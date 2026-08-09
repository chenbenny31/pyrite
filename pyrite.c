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
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/sizes.h>

#include "uapi/pyrite.h"

#define PYRITE_MAX_DEV	16
#define PYRITE_DEFAULT_CAP_MB	8192

static int ndev = 4;
module_param(ndev, int, 0444);
MODULE_PARM_DESC(ndev, "number of mock GPU devices (1..16, default 4)");

static unsigned long capacity_mb[PYRITE_MAX_DEV];
static int capacity_cnt;
module_param_array(capacity_mb, ulong, &capacity_cnt, 0444);
MODULE_PARM_DESC(capacity_mb, "per-device capacity in MiB (default 8192; 0 or unset = default)");

struct pyrite_fctx;

struct pyrite_alloc {
	u64 id;
	u64 bytes;	/* rounded */
	pid_t tgid;	/* task_tgid_nr(current) at ALLOC */
	struct pyrite_fctx *owner;
	struct list_head node;
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
};

struct pyrite_fctx {
	struct pyrite_dev *dev;
};

static int pyrite_ndev;
static struct pyrite_dev *devs;
static dev_t pyrite_devt;
static struct cdev pyrite_cdev;
static struct class *pyrite_class;

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

static long pyrite_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct pyrite_fctx *fctx = filp->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case PYRITE_IOC_ALLOC:
		return pyrite_ioctl_alloc(fctx, uarg);
	case PYRITE_IOC_FREE:
		return pyrite_ioctl_free(fctx, uarg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations pyrite_fops = {
	.owner		= THIS_MODULE,
	.open		= pyrite_open,
	.release	= pyrite_release,
	.unlocked_ioctl	= pyrite_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static int __init pyrite_init(void)
{
	int i, ret;

	if (ndev < 1 || ndev > PYRITE_MAX_DEV) {
		pr_err("ndev must be 1..%d\n", PYRITE_MAX_DEV);
		return -EINVAL;
	}
	pyrite_ndev = ndev;

	devs = kcalloc(pyrite_ndev, sizeof(*devs), GFP_KERNEL);
	if (!devs)
		return -ENOMEM;

	ret = alloc_chrdev_region(&pyrite_devt, 0, pyrite_ndev, "pyrite");
	if (ret)
		goto err_free_devs;

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

	}

	cdev_init(&pyrite_cdev, &pyrite_fops);
	pyrite_cdev.owner = THIS_MODULE;
	ret = cdev_add(&pyrite_cdev, pyrite_devt, pyrite_ndev);
	if (ret)
		goto err_unreg_region;

	pyrite_class = class_create("pyrite");
	if (IS_ERR(pyrite_class)) {
		ret = PTR_ERR(pyrite_class);
		goto err_del_cdev;
	}

	for (i = 0; i < pyrite_ndev; i++) {
		struct device *d;

		d = device_create_with_groups(pyrite_class, NULL,
				MKDEV(MAJOR(pyrite_devt), i),
				&devs[i], NULL, "pyrite%d", i);
		if (IS_ERR(d)) {
			ret = PTR_ERR(d);
			goto err_destroy_devices;
		}
		devs[i].device = d;
	}

	pr_info("loaded: %d device(s), major %d\n",
		pyrite_ndev, MAJOR(pyrite_devt));

	return 0;

err_destroy_devices:
	while (--i >= 0)
		device_destroy(pyrite_class, MKDEV(MAJOR(pyrite_devt), i));
	class_destroy(pyrite_class);
err_del_cdev:
	cdev_del(&pyrite_cdev);
err_unreg_region:
	unregister_chrdev_region(pyrite_devt, pyrite_ndev);
err_free_devs:
	kfree(devs);

	return ret;
}

static void __exit pyrite_exit(void)
{
	int i;

	for (i = 0; i < pyrite_ndev; i++)
		device_destroy(pyrite_class, MKDEV(MAJOR(pyrite_devt), i));

	class_destroy(pyrite_class);
	cdev_del(&pyrite_cdev);
	unregister_chrdev_region(pyrite_devt, pyrite_ndev);

	kfree(devs);

	pr_info("unloaded\n");
}

module_init(pyrite_init);
module_exit(pyrite_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Benny Chen");
MODULE_DESCRIPTION("Stateful mock-GPU character device with per-tgid VRAM accounting");
MODULE_VERSION("0.1");
