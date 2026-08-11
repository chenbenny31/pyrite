// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../uapi/pyrite.h"

#define DEV0 "/dev/pyrite0"
#define DEV1 "/dev/pyrite1"

static int failures;

static void report(const char *name, int ok, const char *detail)
{
	if (ok) {
		printf("ok  %s\n", name);
	} else {
		printf("FAIL %s: %s\n", name, detail);
		failures++;
	}
}

static long do_alloc(int fd, unsigned long long bytes,
		unsigned long long *id, unsigned long long *rounded)
{
	struct pyrite_alloc_req req;

	memset(&req, 0, sizeof(req));
	req.bytes = bytes;

	if (ioctl(fd, PYRITE_IOC_ALLOC, &req) < 0)
		return -errno;

	if (id)
		*id = req.alloc_id;
	if (rounded)
		*rounded = req.bytes;

	return 0;
}

static long do_free(int fd, unsigned long long id)
{
	struct pyrite_free_req req;

	memset(&req, 0, sizeof(req));
	req.alloc_id = id;

	if (ioctl(fd, PYRITE_IOC_FREE, &req) < 0)
		return -errno;

	return 0;
}

static long query_device(int fd, struct pyrite_device_info *info)
{
	memset(info, 0, sizeof(*info));

	if (ioctl(fd, PYRITE_IOC_QUERY_DEVICE, info) < 0)
		return -errno;

	return 0;
}

static long read_sysfs_u64(int index, const char *attr, unsigned long long *out)
{
	char path[128];
	FILE *f;
	int n;

	snprintf(path, sizeof(path), "/sys/class/pyrite/pyrite%d/%s", index, attr);

	f = fopen(path, "r");
	if (!f)
		return -errno;

	n = fscanf(f, "%llu", out);
	fclose(f);

	return n == 1 ? 0 : -EIO;
}

static void t_rounding(void)
{
	unsigned long long id, rounded, used_mb;
	int fd;
	long ret;

	fd = open(DEV0, O_RDWR);
	if (fd < 0) {
		report("rounding", 0, "open failed");
		return;
	}

	ret = do_alloc(fd, 1, &id, &rounded);
	if (ret) {
		report("rounding", 0, "alloc failed");
		close(fd);
		return;
	}

	if (rounded != 2ULL * 1024 * 1024) {
		report("rounding", 0, "not rounded to 2 MiB");
		close(fd);
		return;
	}

	if (read_sysfs_u64(0, "used_mb", &used_mb) || used_mb != 2) {
		report("rounding", 0, "used_mb != 2");
		close(fd);
		return;
	}

	close(fd);
	report("rounding", 1, NULL);
}

static void t_einval(void)
{
	int fd_a, fd_b;
	unsigned long long id;

	fd_a = open(DEV0, O_RDWR);
	fd_b = open(DEV0, O_RDWR);
	if (fd_a < 0 || fd_b < 0) {
		report("einval", 0, "open failed");
		goto out;
	}

	if (do_alloc(fd_a, 0, NULL, NULL) != -EINVAL) {
		report("einval", 0, "zero-byte alloc not EINVAL");
		goto out;
	}

	if (do_free(fd_a, 999999) != -EINVAL) {
		report("einval", 0, "bogus free not EINVAL");
		goto out;
	}

	if (do_alloc(fd_a, 1, &id, NULL)) {
		report("einval", 0, "setup alloc failed");
		goto out;
	}

	/* second fd, same process: still not the owner */
	if (do_free(fd_b, id) != -EINVAL) {
		report("einval", 0, "cross-fd free not EINVAL");
		goto out;
	}

	report("einval", 1, NULL);
out:
	if (fd_a >= 0)
		close(fd_a);
	if (fd_b >= 0)
		close(fd_b);
}

static void t_enomem(void)
{
	struct pyrite_device_info info;
	unsigned long long cap, used_mb;
	int fd;

	fd = open(DEV1, O_RDWR);
	if (fd < 0) {
		report("enomem", 0, "open failed");
		return;
	}

	if (query_device(fd, &info)) {
		report("enomem", 0, "query failed");
		close(fd);
		return;
	}
	cap = info.capacity_bytes;

	if (do_alloc(fd, cap + 1, NULL, NULL) != -ENOMEM) {
		report("enomem", 0, "oversize alloc not ENOMEM");
		close(fd);
		return;
	}

	if (do_alloc(fd, cap, NULL, NULL)) {
		report("enomem", 0, "fill to capacity failed");
		close(fd);
		return;
	}

	if (do_alloc(fd, 1, NULL, NULL) != -ENOMEM) {
		report("enomem", 0, "overcommit not ENOMEM");
		close(fd);
		return;
	}

	close(fd);

	if (read_sysfs_u64(1, "used_mb", &used_mb) || used_mb != 0) {
		report("enomem", 0, "used not zero after close");
		return;
	}

	report("enomem", 1, NULL);
}

int main(void)
{
	t_rounding();
	t_einval();
	t_enomem();

	printf("%d failure(s)\n", failures);
	return failures;
}
