// SPDX-License-Identifier: GPL-2.0-only
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../uapi/pyrite.h"

#define DEV0	"/dev/pyrite0"
#define DEV1	"/dev/pyrite1"
#define DEV2	"/dev/pyrite2"
#define SZ_4M	(4ULL * 1024 * 1024)

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

static long query_procs(int fd, struct pyrite_procs_query *q)
{
	if (ioctl(fd, PYRITE_IOC_QUERY_PROCS, q) < 0)
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

static void t_procs_shape(void)
{
	struct pyrite_procs_query q;
	struct pyrite_proc_entry entries[8];
	int pipefd[2];
	pid_t kids[2];
	char sync;
	int fd = -1, i, n_ready = 0;

	if (pipe(pipefd) < 0) {
		report("procs_shape", 0, "pipe failed");
		return;
	}

	for (i = 0; i < 2; i++) {
		kids[i] = fork();
		if (kids[i] < 0) {
			report("procs_shape", 0, "fork failed");
			return;
		}

		if (kids[i] == 0) {
			int cfd;

			close(pipefd[0]);

			/* open AFTER fork: no parent copy of this file */
			cfd = open(DEV2, O_RDWR);
			if (cfd < 0 || do_alloc(cfd, SZ_4M, NULL, NULL))
				_exit(1);

			if (write(pipefd[1], "r", 1) != 1)
				_exit(1);

			pause();
			_exit(0);
		}
	}

	close(pipefd[1]);

	while (n_ready < 2 && read(pipefd[0], &sync, 1) == 1)
		n_ready++;

	if (n_ready != 2) {
		report("procs_shape", 0, "children not ready");
		goto out_kill;
	}

	fd = open(DEV2, O_RDWR);
	if (fd < 0) {
		report("procs_shape", 0, "open failed");
		goto out_kill;
	}

	/* probe: buf_ptr == 0 */
	memset(&q, 0, sizeof(q));
	if (query_procs(fd, &q) != -ERANGE || q.count != 2) {
		report("procs_shape", 0, "probe not ERANGE/2");
		close(fd);
		goto out_kill;
	}

	/* short buffer */
	memset(&q, 0, sizeof(q));
	q.count = 1;
	q.buf_ptr = (unsigned long long)(uintptr_t)entries;
	if (query_procs(fd, &q) != -ERANGE || q.count != 2) {
		report("procs_shape", 0, "short buf not ERANGE/2");
		close(fd);
		goto out_kill;
	}

	/* full read */
	memset(&q, 0, sizeof(q));
	memset(entries, 0, sizeof(entries));
	q.count = 8;
	q.buf_ptr = (unsigned long long)(uintptr_t)entries;
	if (query_procs(fd, &q) || q.count != 2) {
		report("procs_shape", 0, "full read failed");
		close(fd);
		goto out_kill;
	}

	for (i = 0; i < 2; i++) {
		if (entries[i].tgid != (unsigned long long)kids[0] &&
		entries[i].tgid != (unsigned long long)kids[1]) {
			report("procs_shape", 0, "tgid mismatch");
			close(fd);
			goto out_kill;
		}
		if (entries[i].used_bytes != SZ_4M) {
			report("procs_shape", 0, "bytes mismatch");
			close(fd);
			goto out_kill;
		}
	}

	close(fd);
	report("procs_shape", 1, NULL);

out_kill:
	for (i = 0; i < 2; i++) {
		kill(kids[i], SIGKILL);
		waitpid(kids[i], NULL, 0);
	}
	close(pipefd[0]);
}

int main(void)
{
	t_rounding();
	t_einval();
	t_enomem();
	t_procs_shape();

	printf("%d failure(s)\n", failures);
	return failures;
}
