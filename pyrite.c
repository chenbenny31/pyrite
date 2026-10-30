// SPDX-License-Identifier: GPL-2.0-only
/*
 * pyrite - stateful mock-GPU character device
 *
 * Presents N fake GPUs with per-tgid VRAM accounting.
 * Allocations are bookkeeping only: no memory is reserved and no hardware is touched.
 */

#define pr_fmt(fmt) "pyrite: " fmt

#include <linux/align.h>
#include <linux/cdev.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/prandom.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uuid.h>

#include "uapi/pyrite.h"

#define PYRITE_MAX_DEV	16
#define PYRITE_DEFAULT_CAP_MB	8192
#define PYRITE_INJECT_MAX	32
#define PYRITE_SCRATCH_MIN_KB	4
#define PYRITE_SCRATCH_MAX_KB	16384
#define PYRITE_MAX_STUCK	64
#define PYRITE_PPM		1000000
#define PYRITE_XID_DBE		48	/* NVIDIA's Xid for an uncorrectable ECC error */

static int ndev = 4;
module_param(ndev, int, 0444);
MODULE_PARM_DESC(ndev, "number of mock GPU devices (1..16, default 4)");

static unsigned long capacity_mb[PYRITE_MAX_DEV];
static int capacity_cnt;
module_param_array(capacity_mb, ulong, &capacity_cnt, 0444);
MODULE_PARM_DESC(capacity_mb, "per-device capacity in MiB (default 8192; 0 or unset = default)");

static unsigned long long uuid_seed;
module_param(uuid_seed, ullong, 0444);
MODULE_PARM_DESC(uuid_seed, "seed for device UUIDs (default 0; give each node its own)");

static unsigned int scratch_kb = 1024;
module_param(scratch_kb, uint, 0444);
MODULE_PARM_DESC(scratch_kb, "per-device scratch memory in KiB for read/write tests (4..16384, default 1024)");

struct pyrite_fctx;

struct pyrite_alloc {
	u64 id;
	u64 bytes;	/* rounded */
	pid_t tgid;	/* task_tgid_nr(current) at ALLOC */
	struct pyrite_fctx *owner;
	struct list_head node;
};

enum pyrite_health {
	PYRITE_HEALTH_OK,	/* 0, so kcalloc'd devices start healthy */
	PYRITE_HEALTH_XID,	/* fatal Xid reported; ALLOC fails */
	PYRITE_HEALTH_LOST,	/* fell off the bus; every ioctl fails */
};

/* How a memory bit error shows to a reader */
enum pyrite_ecc {
	PYRITE_ECC_CE,	/* corrected: data right, ecc_ce_count goes up */
	PYRITE_ECC_UE,	/* uncorrected: read fails, health turns xid */
	PYRITE_ECC_SDC,	/* silent: data wrong, nothing reported */
	PYRITE_ECC_NR,
};

static const char * const pyrite_ecc_names[] = {
	[PYRITE_ECC_CE]		= "ce",
	[PYRITE_ECC_UE]		= "ue",
	[PYRITE_ECC_SDC]	= "sdc",
};

/* A scratch cell whose bit always reads as value */
struct pyrite_stuck {
	u32 offset;
	u8 bit;
	u8 value;
	u8 ecc;		/* enum pyrite_ecc */
};

struct pyrite_dev {
	struct device *device;
	struct mutex lock;
	u64 capacity;
	u64 used;
	u64 next_alloc_id;	/* starts at 1; 0 never valid */
	u64 alloc_count;
	struct list_head allocs;
	u32 util;	/* raw, as of last_op */
	unsigned long last_op;	/* jiffies */
	int index;
	uuid_t uuid;	/* immutable after init */
	enum pyrite_health health;
	u32 last_xid;	/* 0 = none since load or reset */
	u8 *scratch;	/* pyrite_scratch_size bytes, under lock */

	/* Memory faults and their counters, all under lock */
	struct pyrite_stuck stuck[PYRITE_MAX_STUCK];
	unsigned int nr_stuck;
	u32 rate[PYRITE_ECC_NR];	/* ppm chance of a bit error per page read */
	struct rnd_state fault_rnd;
	u64 ecc_ce_count;
	u64 ecc_ue_count;
	u64 sdc_count;	/* debugfs only: silent errors handed to readers */
};

struct pyrite_fctx {
	struct pyrite_dev *dev;
};

static int pyrite_ndev;
static struct pyrite_dev *devs;
static dev_t pyrite_devt;
static struct cdev pyrite_cdev;
static struct class *pyrite_class;
static struct dentry *pyrite_debugfs;
static loff_t pyrite_scratch_size;

/* Caller holds dev->lock. */
static u32 pyrite_util_read(const struct pyrite_dev *dev)
{
	unsigned long elapsed = (jiffies - dev->last_op) / HZ;

	if (elapsed >= 32)
		return 0;

	return dev->util >> elapsed;
}

/* Caller holds dev->lock. */
static void pyrite_util_bump(struct pyrite_dev *dev)
{
	dev->util = min_t(u32, 100, pyrite_util_read(dev) + 25);
	dev->last_op = jiffies;
}

static ssize_t capacity_mb_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	struct pyrite_dev *dev = dev_get_drvdata(d);

	/* capacity is immutable after init; safe to read unlocked */
	return sysfs_emit(buf, "%llu\n", dev->capacity >> 20);
}
static DEVICE_ATTR_RO(capacity_mb);

static ssize_t used_mb_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	struct pyrite_dev *dev = dev_get_drvdata(d);
	u64 val;

	mutex_lock(&dev->lock);
	val = dev->used >> 20;
	mutex_unlock(&dev->lock);

	return sysfs_emit(buf, "%llu\n", val);
}
static DEVICE_ATTR_RO(used_mb);

static ssize_t util_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	struct pyrite_dev *dev = dev_get_drvdata(d);
	u32 val;

	mutex_lock(&dev->lock);
	val = pyrite_util_read(dev);
	mutex_unlock(&dev->lock);

	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(util);

static ssize_t numa_node_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0\n");
}
static DEVICE_ATTR_RO(numa_node);

static ssize_t pcie_addr_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	struct pyrite_dev *dev = dev_get_drvdata(d);

	return sysfs_emit(buf, "0000:%02x:00.0\n", dev->index);
}
static DEVICE_ATTR_RO(pcie_addr);

static ssize_t uuid_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	struct pyrite_dev *dev = dev_get_drvdata(d);

	/* uuid is immutable after init; safe to read unlocked */
	return sysfs_emit(buf, "GPU-%pUb\n", &dev->uuid);
}
static DEVICE_ATTR_RO(uuid);

static const char * const pyrite_health_names[] = {
	[PYRITE_HEALTH_OK]	= "ok",
	[PYRITE_HEALTH_XID]	= "xid",
	[PYRITE_HEALTH_LOST]	= "lost",
};

static ssize_t health_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	struct pyrite_dev *dev = dev_get_drvdata(d);
	enum pyrite_health val;

	mutex_lock(&dev->lock);
	val = dev->health;
	mutex_unlock(&dev->lock);

	return sysfs_emit(buf, "%s\n", pyrite_health_names[val]);
}
static DEVICE_ATTR_RO(health);

static ssize_t last_xid_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	struct pyrite_dev *dev = dev_get_drvdata(d);
	u32 val;

	mutex_lock(&dev->lock);
	val = dev->last_xid;
	mutex_unlock(&dev->lock);

	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(last_xid);

static ssize_t ecc_ce_count_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	struct pyrite_dev *dev = dev_get_drvdata(d);
	u64 val;

	mutex_lock(&dev->lock);
	val = dev->ecc_ce_count;
	mutex_unlock(&dev->lock);

	return sysfs_emit(buf, "%llu\n", val);
}
static DEVICE_ATTR_RO(ecc_ce_count);

static ssize_t ecc_ue_count_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	struct pyrite_dev *dev = dev_get_drvdata(d);
	u64 val;

	mutex_lock(&dev->lock);
	val = dev->ecc_ue_count;
	mutex_unlock(&dev->lock);

	return sysfs_emit(buf, "%llu\n", val);
}
static DEVICE_ATTR_RO(ecc_ue_count);

/*
 * Writing 1 resets the device the way nvidia-smi -r does: health goes back to
 * ok, but only once nothing is allocated, so the work on it must leave first.
 * Memory errors stay, since a reset does not fix bad memory. The debugfs
 * reset is a test knob and skips the allocation check.
 */
static ssize_t reset_store(struct device *d, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct pyrite_dev *dev = dev_get_drvdata(d);
	unsigned int val;
	int ret = 0;

	if (kstrtouint(buf, 10, &val) || val != 1)
		return -EINVAL;

	mutex_lock(&dev->lock);
	if (dev->alloc_count) {
		ret = -EBUSY;
	} else {
		dev->health = PYRITE_HEALTH_OK;
		dev->last_xid = 0;
	}
	mutex_unlock(&dev->lock);

	if (ret)
		return ret;

	dev_info(dev->device, "reset\n");
	return count;
}
static DEVICE_ATTR_WO(reset);

static ssize_t scratch_kb_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", scratch_kb);
}
static DEVICE_ATTR_RO(scratch_kb);

static struct attribute *pyrite_attrs[] = {
	&dev_attr_capacity_mb.attr,
	&dev_attr_used_mb.attr,
	&dev_attr_util.attr,
	&dev_attr_numa_node.attr,
	&dev_attr_pcie_addr.attr,
	&dev_attr_uuid.attr,
	&dev_attr_health.attr,
	&dev_attr_last_xid.attr,
	&dev_attr_scratch_kb.attr,
	&dev_attr_ecc_ce_count.attr,
	&dev_attr_ecc_ue_count.attr,
	&dev_attr_reset.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pyrite);

static int pyrite_open(struct inode *inode, struct file *filp)
{
	struct pyrite_fctx *fctx;
	unsigned int minor = iminor(inode);

	if (minor >= (unsigned int)pyrite_ndev)
		return -ENODEV;

	fctx = kzalloc(sizeof(*fctx), GFP_KERNEL);
	if (!fctx)
		return -ENOMEM;

	fctx->dev = &devs[minor];
	filp->private_data = fctx;

	return 0;
}

static int pyrite_release(struct inode *inode, struct file *filp)
{
	struct pyrite_fctx *fctx = filp->private_data;
	struct pyrite_dev *dev = fctx->dev;
	struct pyrite_alloc *alloc, *tmp;

	mutex_lock(&dev->lock);

	list_for_each_entry_safe(alloc, tmp, &dev->allocs, node) {
		if (alloc->owner != fctx)
			continue;

		list_del(&alloc->node);
		dev->used -= alloc->bytes;
		dev->alloc_count--;
		kfree(alloc);
	}

	mutex_unlock(&dev->lock);

	kfree(fctx);

	return 0;
}

static long pyrite_ioctl_alloc(struct pyrite_fctx *fctx, void __user *uarg)
{
	struct pyrite_dev *dev = fctx->dev;
	struct pyrite_alloc_req req;
	struct pyrite_alloc *alloc;
	u64 rounded;
	long ret = 0;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;

	if (req.bytes == 0)
		return -EINVAL;

	/* capacity is immutable after init; safe to read unlocked */
	if (req.bytes > dev->capacity)
		return -ENOMEM;

	rounded = ALIGN(req.bytes, SZ_2M);

	alloc = kzalloc(sizeof(*alloc), GFP_KERNEL);
	if (!alloc)
		return -ENOMEM;

	mutex_lock(&dev->lock);

	if (dev->health == PYRITE_HEALTH_LOST) {
		ret = -ENODEV;
		goto out_unlock;
	}

	if (dev->health == PYRITE_HEALTH_XID) {
		ret = -EIO;
		goto out_unlock;
	}

	if (dev->used + rounded > dev->capacity) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	alloc->id = dev->next_alloc_id++;
	alloc->bytes = rounded;
	alloc->tgid = task_tgid_nr(current);
	alloc->owner = fctx;
	list_add_tail(&alloc->node, &dev->allocs);

	dev->used += rounded;
	dev->alloc_count++;
	pyrite_util_bump(dev);

	req.bytes = rounded;
	req.alloc_id = alloc->id;

out_unlock:
	mutex_unlock(&dev->lock);

	if (ret) {
		kfree(alloc);
		return ret;
	}

	if (copy_to_user(uarg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long pyrite_ioctl_free(struct pyrite_fctx *fctx, void __user *uarg)
{
	struct pyrite_dev *dev = fctx->dev;
	struct pyrite_free_req req;
	struct pyrite_alloc *alloc;
	struct pyrite_alloc *found = NULL;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&dev->lock);

	if (dev->health == PYRITE_HEALTH_LOST) {
		mutex_unlock(&dev->lock);
		return -ENODEV;
	}

	list_for_each_entry(alloc, &dev->allocs, node) {
		if (alloc->id == req.alloc_id && alloc->owner == fctx) {
			found = alloc;
			break;
		}
	}

	if (!found) {
		mutex_unlock(&dev->lock);
		return -EINVAL;
	}

	list_del(&found->node);
	dev->used -= found->bytes;
	dev->alloc_count--;
	pyrite_util_bump(dev);

	mutex_unlock(&dev->lock);

	kfree(found);

	return 0;
}

static long pyrite_ioctl_query_device(struct pyrite_fctx *fctx, void __user *uarg)
{
	struct pyrite_dev *dev = fctx->dev;
	struct pyrite_device_info info;

	mutex_lock(&dev->lock);

	if (dev->health == PYRITE_HEALTH_LOST) {
		mutex_unlock(&dev->lock);
		return -ENODEV;
	}

	info.capacity_bytes = dev->capacity;
	info.used_bytes = dev->used;
	info.util_pct = pyrite_util_read(dev);
	info.alloc_count = dev->alloc_count;
	mutex_unlock(&dev->lock);

	if (copy_to_user(uarg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static long pyrite_ioctl_query_procs(struct pyrite_fctx *fctx, void __user *uarg)
{
	struct pyrite_dev *dev = fctx->dev;
	struct pyrite_procs_query q;
	struct pyrite_proc_entry *scratch = NULL;
	struct pyrite_alloc *alloc;
	u64 needed = 0;
	u64 i;
	long ret = 0;

	if (copy_from_user(&q, uarg, sizeof(q)))
		return -EFAULT;

	mutex_lock(&dev->lock);

	if (dev->health == PYRITE_HEALTH_LOST) {
		mutex_unlock(&dev->lock);
		return -ENODEV;
	}

	if (dev->alloc_count) {
		scratch = kmalloc_array(dev->alloc_count, sizeof(*scratch), GFP_KERNEL);
		if (!scratch) {
			mutex_unlock(&dev->lock);
			return -ENOMEM;
		}
	}

	list_for_each_entry(alloc, &dev->allocs, node) {
		for (i = 0; i < needed; i++) {
			if (scratch[i].tgid == (u64)alloc->tgid) {
				scratch[i].used_bytes += alloc->bytes;
				break;
			}
		}

		if (i == needed) {
			scratch[needed].tgid = alloc->tgid;
			scratch[needed].used_bytes = alloc->bytes;
			needed++;
		}
	}

	mutex_unlock(&dev->lock);

	if (needed == 0) {
		q.count = 0;
		goto out_copy;
	}

	if (q.buf_ptr == 0 || q.count < needed) {
		q.count = needed;
		ret = -ERANGE;
		goto out_copy;
	}

	if (copy_to_user(u64_to_user_ptr(q.buf_ptr), scratch,
				needed * sizeof(*scratch))) {
		kfree(scratch);
		return -EFAULT;
	}

	q.count = needed;

out_copy:
	kfree(scratch);

	if (copy_to_user(uarg, &q, sizeof(q)))
		return -EFAULT;

	return ret;
}

static long pyrite_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct pyrite_fctx *fctx = filp->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case PYRITE_IOC_ALLOC:
		return pyrite_ioctl_alloc(fctx, uarg);
	case PYRITE_IOC_FREE:
		return pyrite_ioctl_free(fctx, uarg);
	case PYRITE_IOC_QUERY_DEVICE:
		return pyrite_ioctl_query_device(fctx, uarg);
	case PYRITE_IOC_QUERY_PROCS:
		return pyrite_ioctl_query_procs(fctx, uarg);
	default:
		return -ENOTTY;
	}
}

/*
 * Caller holds dev->lock, or dev is not visible yet. Removes all memory
 * faults, zeroes the counters, and restarts the transient error stream from
 * the device's UUID, so the same reads give the same errors after every clear.
 */
static void pyrite_clear_faults(struct pyrite_dev *dev)
{
	u64 seed;

	dev->nr_stuck = 0;
	memset(dev->rate, 0, sizeof(dev->rate));
	dev->ecc_ce_count = 0;
	dev->ecc_ue_count = 0;
	dev->sdc_count = 0;

	memcpy(&seed, dev->uuid.b, sizeof(seed));
	prandom_seed_state(&dev->fault_rnd, seed);
}

/*
 * Caller holds dev->lock. One bit error of class ecc at buf[off]. Returns
 * true for an uncorrected error, which fails the read.
 */
static bool pyrite_bit_error(struct pyrite_dev *dev, u8 *buf, size_t off,
		u8 mask, int ecc)
{
	switch (ecc) {
	case PYRITE_ECC_CE:
		dev->ecc_ce_count++;
		return false;
	case PYRITE_ECC_UE:
		dev->ecc_ue_count++;
		return true;
	default:
		buf[off] ^= mask;
		dev->sdc_count++;
		return false;
	}
}

/*
 * Caller holds dev->lock. buf holds len bytes just copied from scratch at pos,
 * within one page. Applies the stuck cells in range and the transient rates
 * to buf only; scratch keeps what was written. Returns true if an uncorrected
 * error hit.
 */
static bool pyrite_apply_faults(struct pyrite_dev *dev, u8 *buf, loff_t pos,
		size_t len)
{
	bool ue = false;
	unsigned int i;

	for (i = 0; i < dev->nr_stuck; i++) {
		const struct pyrite_stuck *s = &dev->stuck[i];
		u8 mask = BIT(s->bit);

		if (s->offset < pos || s->offset >= pos + len)
			continue;
		/* a stuck cell shows only when the stored bit differs */
		if (!!(buf[s->offset - pos] & mask) == s->value)
			continue;
		ue |= pyrite_bit_error(dev, buf, s->offset - pos, mask, s->ecc);
	}

	for (i = 0; i < PYRITE_ECC_NR; i++) {
		u32 bit;

		if (!dev->rate[i] ||
				prandom_u32_state(&dev->fault_rnd) % PYRITE_PPM >= dev->rate[i])
			continue;
		bit = prandom_u32_state(&dev->fault_rnd) % (len * 8);
		ue |= pyrite_bit_error(dev, buf, bit / 8, BIT(bit % 8), i);
	}

	return ue;
}

static loff_t pyrite_llseek(struct file *filp, loff_t off, int whence)
{
	return fixed_size_llseek(filp, off, whence, pyrite_scratch_size);
}

/*
 * Scratch reads and writes go one page of scratch at a time through a bounce
 * page, so no copy_*_user runs under the lock. A short count is returned if a
 * later page fails. Memory faults apply to each page as it is read.
 */
static ssize_t pyrite_read(struct file *filp, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	struct pyrite_fctx *fctx = filp->private_data;
	struct pyrite_dev *dev = fctx->dev;
	loff_t pos = *ppos;
	size_t done = 0;
	ssize_t ret = 0;
	void *bounce;
	bool ue;

	if (pos < 0)
		return -EINVAL;
	if (pos >= pyrite_scratch_size || count == 0)
		return 0;
	count = min_t(size_t, count, pyrite_scratch_size - pos);

	bounce = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	while (done < count) {
		size_t len = min_t(size_t, count - done, PAGE_SIZE - offset_in_page(pos));

		mutex_lock(&dev->lock);
		if (dev->health == PYRITE_HEALTH_LOST) {
			mutex_unlock(&dev->lock);
			ret = -ENODEV;
			break;
		}
		memcpy(bounce, dev->scratch + pos, len);
		ue = pyrite_apply_faults(dev, bounce, pos, len);
		if (ue) {
			dev->health = PYRITE_HEALTH_XID;
			dev->last_xid = PYRITE_XID_DBE;
		}
		mutex_unlock(&dev->lock);

		if (ue) {
			dev_warn(dev->device, "Xid %u: uncorrectable ECC error\n",
				PYRITE_XID_DBE);
			ret = -EIO;
			break;
		}

		if (copy_to_user(ubuf + done, bounce, len)) {
			ret = -EFAULT;
			break;
		}
		done += len;
		pos += len;
	}

	kfree(bounce);
	*ppos = pos;

	return done ? done : ret;
}

static ssize_t pyrite_write(struct file *filp, const char __user *ubuf,
		size_t count, loff_t *ppos)
{
	struct pyrite_fctx *fctx = filp->private_data;
	struct pyrite_dev *dev = fctx->dev;
	loff_t pos = *ppos;
	size_t done = 0;
	ssize_t ret = 0;
	void *bounce;

	if (pos < 0)
		return -EINVAL;
	if (count == 0)
		return 0;
	if (pos >= pyrite_scratch_size)
		return -ENOSPC;
	count = min_t(size_t, count, pyrite_scratch_size - pos);

	bounce = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	while (done < count) {
		size_t len = min_t(size_t, count - done, PAGE_SIZE - offset_in_page(pos));

		if (copy_from_user(bounce, ubuf + done, len)) {
			ret = -EFAULT;
			break;
		}

		mutex_lock(&dev->lock);
		if (dev->health == PYRITE_HEALTH_LOST) {
			mutex_unlock(&dev->lock);
			ret = -ENODEV;
			break;
		}
		memcpy(dev->scratch + pos, bounce, len);
		mutex_unlock(&dev->lock);

		done += len;
		pos += len;
	}

	kfree(bounce);
	*ppos = pos;

	return done ? done : ret;
}

static const struct file_operations pyrite_fops = {
	.owner		= THIS_MODULE,
	.open		= pyrite_open,
	.release	= pyrite_release,
	.llseek		= pyrite_llseek,
	.read		= pyrite_read,
	.write		= pyrite_write,
	.unlocked_ioctl	= pyrite_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

/* Split s at single spaces into exactly n words. */
static int pyrite_split(char *s, char **words, int n)
{
	int i;

	s = strim(s);
	for (i = 0; i < n; i++) {
		words[i] = strsep(&s, " ");
		if (!words[i] || !*words[i])
			return -EINVAL;
	}

	return s ? -EINVAL : 0;
}

/* "stuck <ce|ue|sdc> <offset> <bit> <0|1>" */
static int pyrite_inject_stuck(struct pyrite_dev *dev, char *args)
{
	unsigned int off, bit, val;
	char *w[4];
	int ecc;
	int ret = 0;

	if (pyrite_split(args, w, 4))
		return -EINVAL;
	ecc = match_string(pyrite_ecc_names, PYRITE_ECC_NR, w[0]);
	if (ecc < 0 ||
			kstrtouint(w[1], 10, &off) || off >= pyrite_scratch_size ||
			kstrtouint(w[2], 10, &bit) || bit > 7 ||
			kstrtouint(w[3], 10, &val) || val > 1)
		return -EINVAL;

	mutex_lock(&dev->lock);
	if (dev->nr_stuck == PYRITE_MAX_STUCK)
		ret = -ENOSPC;
	else
		dev->stuck[dev->nr_stuck++] = (struct pyrite_stuck) {
			.offset = off, .bit = bit, .value = val, .ecc = ecc,
		};
	mutex_unlock(&dev->lock);

	if (!ret)
		dev_info(dev->device, "stuck %s cell: byte %u bit %u reads %u\n",
			pyrite_ecc_names[ecc], off, bit, val);
	return ret;
}

/* "rate <ce|ue|sdc> <ppm>" */
static int pyrite_inject_rate(struct pyrite_dev *dev, char *args)
{
	unsigned int ppm;
	char *w[2];
	int ecc;

	if (pyrite_split(args, w, 2))
		return -EINVAL;
	ecc = match_string(pyrite_ecc_names, PYRITE_ECC_NR, w[0]);
	if (ecc < 0 || kstrtouint(w[1], 10, &ppm) || ppm > PYRITE_PPM)
		return -EINVAL;

	mutex_lock(&dev->lock);
	dev->rate[ecc] = ppm;
	mutex_unlock(&dev->lock);

	dev_info(dev->device, "%s errors at %u ppm per page read\n",
		pyrite_ecc_names[ecc], ppm);
	return 0;
}

static ssize_t pyrite_inject_write(struct file *filp, const char __user *ubuf,
		size_t count, loff_t *ppos)
{
	struct pyrite_dev *dev = filp->private_data;
	char cmd[PYRITE_INJECT_MAX];
	u32 xid;
	int ret;

	if (count >= sizeof(cmd))
		return -EINVAL;

	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';

	if (str_has_prefix(cmd, "xid ")) {
		if (kstrtou32(cmd + 4, 10, &xid) || xid == 0)
			return -EINVAL;

		mutex_lock(&dev->lock);
		dev->health = PYRITE_HEALTH_XID;
		dev->last_xid = xid;
		mutex_unlock(&dev->lock);

		dev_warn(dev->device, "Xid %u\n", xid);
	} else if (sysfs_streq(cmd, "lost")) {
		mutex_lock(&dev->lock);
		dev->health = PYRITE_HEALTH_LOST;
		mutex_unlock(&dev->lock);

		dev_err(dev->device, "fell off the bus\n");
	} else if (sysfs_streq(cmd, "reset")) {
		mutex_lock(&dev->lock);
		dev->health = PYRITE_HEALTH_OK;
		dev->last_xid = 0;
		mutex_unlock(&dev->lock);

		dev_info(dev->device, "reset\n");
	} else if (str_has_prefix(cmd, "stuck ")) {
		ret = pyrite_inject_stuck(dev, cmd + 6);
		if (ret)
			return ret;
	} else if (str_has_prefix(cmd, "rate ")) {
		ret = pyrite_inject_rate(dev, cmd + 5);
		if (ret)
			return ret;
	} else if (sysfs_streq(cmd, "clear")) {
		mutex_lock(&dev->lock);
		pyrite_clear_faults(dev);
		mutex_unlock(&dev->lock);

		dev_info(dev->device, "memory faults cleared\n");
	} else {
		return -EINVAL;
	}

	return count;
}

static const struct file_operations pyrite_inject_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= pyrite_inject_write,
};

static int sdc_count_show(struct seq_file *m, void *unused)
{
	struct pyrite_dev *dev = m->private;
	u64 val;

	mutex_lock(&dev->lock);
	val = dev->sdc_count;
	mutex_unlock(&dev->lock);

	seq_printf(m, "%llu\n", val);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sdc_count);

/* Test-only fault injection; pyrite works the same without debugfs */
static void pyrite_debugfs_init(void)
{
	int i;

	pyrite_debugfs = debugfs_create_dir("pyrite", NULL);

	for (i = 0; i < pyrite_ndev; i++) {
		struct dentry *dir;

		dir = debugfs_create_dir(dev_name(devs[i].device), pyrite_debugfs);
		debugfs_create_file("inject", 0200, dir, &devs[i],
				&pyrite_inject_fops);
		/* ground truth for tests, kept out of sysfs so no monitor sees it */
		debugfs_create_file("sdc_count", 0400, dir, &devs[i],
				&sdc_count_fops);
	}
}

static void pyrite_free_scratch(void)
{
	int i;

	for (i = 0; i < pyrite_ndev; i++)
		kvfree(devs[i].scratch);
}

static int __init pyrite_init(void)
{
	struct rnd_state rnd;
	int i, ret;

	if (ndev < 1 || ndev > PYRITE_MAX_DEV) {
		pr_err("ndev must be 1..%d\n", PYRITE_MAX_DEV);
		return -EINVAL;
	}
	if (scratch_kb < PYRITE_SCRATCH_MIN_KB || scratch_kb > PYRITE_SCRATCH_MAX_KB) {
		pr_err("scratch_kb must be %d..%d\n",
			PYRITE_SCRATCH_MIN_KB, PYRITE_SCRATCH_MAX_KB);
		return -EINVAL;
	}
	pyrite_ndev = ndev;
	pyrite_scratch_size = (loff_t)scratch_kb << 10;

	devs = kcalloc(pyrite_ndev, sizeof(*devs), GFP_KERNEL);
	if (!devs)
		return -ENOMEM;

	ret = alloc_chrdev_region(&pyrite_devt, 0, pyrite_ndev, "pyrite");
	if (ret)
		goto err_free_devs;

	prandom_seed_state(&rnd, uuid_seed);

	for (i = 0; i < pyrite_ndev; i++) {
		struct pyrite_dev *dev = &devs[i];
		unsigned long cap_mb = PYRITE_DEFAULT_CAP_MB;

		if (i < capacity_cnt && capacity_mb[i])
			cap_mb = capacity_mb[i];

		mutex_init(&dev->lock);
		INIT_LIST_HEAD(&dev->allocs);
		dev->capacity = (u64)cap_mb << 20;
		dev->next_alloc_id = 1;
		dev->last_op = jiffies;
		dev->index = i;

		/* same seed, same UUIDs; version and variant bits as in uuid_gen() */
		prandom_bytes_state(&rnd, dev->uuid.b, UUID_SIZE);
		dev->uuid.b[6] = (dev->uuid.b[6] & 0x0f) | 0x40;
		dev->uuid.b[8] = (dev->uuid.b[8] & 0x3f) | 0x80;
		pyrite_clear_faults(dev);

		dev->scratch = kvzalloc(pyrite_scratch_size, GFP_KERNEL);
		if (!dev->scratch) {
			ret = -ENOMEM;
			goto err_free_scratch;
		}
	}

	cdev_init(&pyrite_cdev, &pyrite_fops);
	pyrite_cdev.owner = THIS_MODULE;
	ret = cdev_add(&pyrite_cdev, pyrite_devt, pyrite_ndev);
	if (ret)
		goto err_free_scratch;

	pyrite_class = class_create("pyrite");
	if (IS_ERR(pyrite_class)) {
		ret = PTR_ERR(pyrite_class);
		goto err_del_cdev;
	}

	for (i = 0; i < pyrite_ndev; i++) {
		struct device *d;

		d = device_create_with_groups(pyrite_class, NULL,
				MKDEV(MAJOR(pyrite_devt), i),
				&devs[i], pyrite_groups, "pyrite%d", i);
		if (IS_ERR(d)) {
			ret = PTR_ERR(d);
			goto err_destroy_devices;
		}
		devs[i].device = d;
	}

	pyrite_debugfs_init();

	pr_info("loaded: %d device(s), major %d\n",
		pyrite_ndev, MAJOR(pyrite_devt));

	return 0;

err_destroy_devices:
	while (--i >= 0)
		device_destroy(pyrite_class, MKDEV(MAJOR(pyrite_devt), i));
	class_destroy(pyrite_class);
err_del_cdev:
	cdev_del(&pyrite_cdev);
err_free_scratch:
	pyrite_free_scratch();
	unregister_chrdev_region(pyrite_devt, pyrite_ndev);
err_free_devs:
	kfree(devs);

	return ret;
}

static void __exit pyrite_exit(void)
{
	int i;

	/* waits for inject writes in progress */
	debugfs_remove(pyrite_debugfs);

	for (i = 0; i < pyrite_ndev; i++)
		device_destroy(pyrite_class, MKDEV(MAJOR(pyrite_devt), i));

	class_destroy(pyrite_class);
	cdev_del(&pyrite_cdev);
	unregister_chrdev_region(pyrite_devt, pyrite_ndev);

	pyrite_free_scratch();
	kfree(devs);

	pr_info("unloaded\n");
}

module_init(pyrite_init);
module_exit(pyrite_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Benny Chen");
MODULE_DESCRIPTION("Stateful mock-GPU character device with per-tgid VRAM accounting");
MODULE_VERSION("0.1");
