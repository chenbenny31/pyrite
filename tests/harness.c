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
#include <time.h>
#include <pthread.h>

#include "../uapi/pyrite.h"

#define DEV0	"/dev/pyrite0"
#define DEV1	"/dev/pyrite1"
#define DEV2	"/dev/pyrite2"
#define DEV3	"/dev/pyrite3"
#define SZ_4M	(4ULL * 1024 * 1024)
#define HAMMER_ITERS	10000

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
	if (id)
		*id = 0;
	if (rounded)
		*rounded = 0;

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

static long read_sysfs_str(int index, const char *attr, char *buf, size_t len)
{
	char path[128];
	FILE *f;
	char *line;

	snprintf(path, sizeof(path), "/sys/class/pyrite/pyrite%d/%s", index, attr);

	f = fopen(path, "r");
	if (!f)
		return -errno;

	line = fgets(buf, len, f);
	fclose(f);

	if (!line)
		return -EIO;

	buf[strcspn(buf, "\n")] = '\0';

	return 0;
}

static long inject(int index, const char *cmd)
{
	char path[128];
	long ret = 0;
	int fd;

	snprintf(path, sizeof(path), "/sys/kernel/debug/pyrite/pyrite%d/inject", index);

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;

	if (write(fd, cmd, strlen(cmd)) < 0)
		ret = -errno;

	close(fd);

	return ret;
}

static int health_is(int index, const char *want, unsigned long long want_xid)
{
	unsigned long long xid;
	char health[16];

	return !read_sysfs_str(index, "health", health, sizeof(health)) &&
		!strcmp(health, want) &&
		!read_sysfs_u64(index, "last_xid", &xid) && xid == want_xid;
}

/* "GPU-" + RFC 4122 v4: xxxxxxxx-xxxx-4xxx-[89ab]xxx-xxxxxxxxxxxx, lowercase */
static int uuid_ok(const char *s)
{
	int i;

	if (strlen(s) != 40 || strncmp(s, "GPU-", 4))
		return 0;

	s += 4;
	for (i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (s[i] != '-')
				return 0;
		} else if (!strchr("0123456789abcdef", s[i])) {
			return 0;
		}
	}

	return s[14] == '4' && strchr("89ab", s[19]);
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

static void t_kill_cleanup(void)
{
	unsigned long long used0, used3;
	struct timespec t0, t1;
	int pipefd[2];
	pid_t kid;
	char sync;
	double ms;
	int i;

	if (pipe(pipefd) < 0) {
		report("kill_cleanup", 0, "pipe failed");
		return;
	}

	kid = fork();
	if (kid < 0) {
		report("kill_cleanup", 0, "fork failed");
		return;
	}

	if (kid == 0) {
		int fd0, fd3;

		close(pipefd[0]);

		/* both opened AFTER fork: parent holds no copy */
		fd0 = open(DEV0, O_RDWR);
		fd3 = open(DEV3, O_RDWR);
		if (fd0 < 0 || fd3 < 0)
			_exit(1);

		if (do_alloc(fd0, SZ_4M, NULL, NULL) || do_alloc(fd3, SZ_4M, NULL, NULL))
			_exit(1);

		if (write(pipefd[1], "r", 1) != 1)
			_exit(1);

		pause();
		_exit(0);
	}

	close(pipefd[1]);

	if (read(pipefd[0], &sync, 1) != 1) {
		report("kill_cleanup", 0, "child not ready");
		goto out;
	}

	if (read_sysfs_u64(0, "used_mb", &used0)
			|| read_sysfs_u64(3, "used_mb", &used3)
			|| used0 != 4 || used3 != 4) {
		report("kill_cleanup", 0, "pre-kill accounting wrong");
		goto out;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	kill(kid, SIGKILL);
	waitpid(kid, NULL, 0);

	for (i = 0; i < 2000; i++) {
		if (read_sysfs_u64(0, "used_mb", &used0) ||
				read_sysfs_u64(3, "used_mb", &used3))
			break;

		if (used0 == 0 && used3 == 0)
			break;

		usleep(1000);
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);

	if (used0 != 0 || used3 != 0) {
		report("kill_cleanup", 0, "used not zero within 2s");
		goto out;
	}

	ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000.0;
	printf("    reclaim latency: %.2f ms\n", ms);

	report("kill_cleanup", 1, NULL);

out:
	close(pipefd[0]);
}

static void *hammer_thread(void *arg)
{
	unsigned int seed = (unsigned int)(uintptr_t)arg;
	unsigned long long ids[64];
	int held = 0;
	int fd, i;

	fd = open(DEV0, O_RDWR);
	if (fd < 0)
		return (void *)1;

	for (i = 0; i < HAMMER_ITERS; i++) {
		if (held < 64 && (held == 0 || rand_r(&seed) % 2)) {
			unsigned long long id;

			if (!do_alloc(fd, SZ_4M, &id, NULL))
				ids[held++] = id;
		} else {
			int victim = rand_r(&seed) % held;

			do_free(fd, ids[victim]);
			ids[victim] = ids[--held];
		}
	}

	while (held > 0)
		do_free(fd, ids[--held]);

	close(fd);

	return NULL;
}

static void t_hammer(void)
{
	struct pyrite_device_info info;
	pthread_t th[2];
	void *rv;
	int fd, i;

	for (i = 0; i < 2; i++) {
		if (pthread_create(&th[i], NULL, hammer_thread,
					(void *)(uintptr_t)(i + 1))) {
			report("hammer", 0, "pthread_create failed");
			return;
		}
	}

	for (i = 0; i < 2; i++) {
		pthread_join(th[i], &rv);
		if (rv) {
			report("hammer", 0, "thread open failed");
			return;
		}
	}

	fd = open(DEV0, O_RDWR);
	if (fd < 0 || query_device(fd, &info)) {
		report("hammer", 0, "query failed");
		if (fd >= 0)
			close(fd);
		return;
	}
	close(fd);

	if (info.used_bytes != 0 || info.alloc_count != 0) {
		report("hammer", 0, "state not zero after hammer");
		return;
	}

	report("hammer", 1, NULL);
}

static void t_uuid(void)
{
	char uuid[4][64], again[64];
	int i, j;

	for (i = 0; i < 4; i++) {
		if (read_sysfs_str(i, "uuid", uuid[i], sizeof(uuid[i])) ||
				!uuid_ok(uuid[i])) {
			report("uuid", 0, "malformed uuid");
			return;
		}

		for (j = 0; j < i; j++) {
			if (!strcmp(uuid[i], uuid[j])) {
				report("uuid", 0, "duplicate uuid");
				return;
			}
		}
	}

	for (i = 0; i < 4; i++) {
		if (read_sysfs_str(i, "uuid", again, sizeof(again)) ||
				strcmp(uuid[i], again)) {
			report("uuid", 0, "uuid changed between reads");
			return;
		}
	}

	report("uuid", 1, NULL);
}

static void t_xid(void)
{
	struct pyrite_device_info info;
	unsigned long long id;
	int fd;

	fd = open(DEV1, O_RDWR);
	if (fd < 0) {
		report("xid", 0, "open failed");
		return;
	}

	if (do_alloc(fd, SZ_4M, &id, NULL)) {
		report("xid", 0, "setup alloc failed");
		goto out;
	}

	if (inject(1, "xid 48\n")) {
		report("xid", 0, "inject xid failed");
		goto out;
	}

	if (inject(1, "xid") != -EINVAL || inject(1, "xid 0") != -EINVAL ||
			inject(1, "bogus") != -EINVAL) {
		report("xid", 0, "bad command not EINVAL");
		goto out;
	}

	if (!health_is(1, "xid", 48)) {
		report("xid", 0, "health/last_xid not xid/48");
		goto out;
	}

	if (do_alloc(fd, SZ_4M, NULL, NULL) != -EIO) {
		report("xid", 0, "alloc not EIO");
		goto out;
	}

	/* work already on the device can still query and free */
	if (query_device(fd, &info) || info.alloc_count != 1 || do_free(fd, id)) {
		report("xid", 0, "query or free failed");
		goto out;
	}

	if (inject(1, "reset") || !health_is(1, "ok", 0)) {
		report("xid", 0, "reset failed");
		goto out;
	}

	if (do_alloc(fd, SZ_4M, NULL, NULL)) {
		report("xid", 0, "alloc after reset failed");
		goto out;
	}

	report("xid", 1, NULL);
out:
	/* close first: reset may later refuse a device with allocations */
	close(fd);
	inject(1, "reset");
}

static void t_lost(void)
{
	struct pyrite_device_info info;
	struct pyrite_procs_query q;
	unsigned long long id, used_mb;
	int fd;

	fd = open(DEV2, O_RDWR);
	if (fd < 0) {
		report("lost", 0, "open failed");
		return;
	}

	if (do_alloc(fd, SZ_4M, &id, NULL)) {
		report("lost", 0, "setup alloc failed");
		close(fd);
		return;
	}

	if (inject(2, "lost\n") || !health_is(2, "lost", 0)) {
		report("lost", 0, "inject lost failed");
		close(fd);
		goto out;
	}

	memset(&q, 0, sizeof(q));
	if (do_alloc(fd, SZ_4M, NULL, NULL) != -ENODEV ||
			do_free(fd, id) != -ENODEV ||
			query_device(fd, &info) != -ENODEV ||
			query_procs(fd, &q) != -ENODEV) {
		report("lost", 0, "ioctl not ENODEV");
		close(fd);
		goto out;
	}

	/* release reclaims even from a lost device */
	close(fd);

	if (read_sysfs_u64(2, "used_mb", &used_mb) || used_mb != 0) {
		report("lost", 0, "used not zero after close");
		goto out;
	}

	report("lost", 1, NULL);
out:
	inject(2, "reset");
}

int main(void)
{
	t_rounding();
	t_einval();
	t_enomem();
	t_procs_shape();
	t_kill_cleanup();
	t_hammer();
	t_uuid();
	t_xid();
	t_lost();

	printf("%d failure(s)\n", failures);
	return failures;
}
