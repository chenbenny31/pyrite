// SPDX-License-Identifier: GPL-2.0-only
/*
 * qualify - known-answer test of pyrite scratch memory
 *
 * Writes four patterns over each device's whole scratch memory and reads each
 * back: all zeros, all ones, seeded random data and its inverse. The solid
 * patterns catch a stuck bit whichever way it is stuck; the random pair varies
 * the data and adds reads, which gives intermittent errors more chances to
 * show. pyrite reports nothing for a silent error, so a mismatch here is the
 * only way to find one.
 *
 * Exit status: 0 every device passed, 1 some data came back wrong, 2 a device
 * failed to open, read or write, 3 bad usage. With several devices the worst
 * result wins.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { PASS, MISMATCH, DEVICE_ERROR, USAGE };

#define NR_PATTERNS	4

static uint64_t rng;

/* xorshift64; rng must not be 0 */
static uint64_t rng_next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

/* Pattern 3 inverts pattern 2 in place, so they must run in order */
static void fill(unsigned char *buf, size_t size, int pattern)
{
	size_t i;

	switch (pattern) {
	case 0:
		memset(buf, 0x00, size);
		break;
	case 1:
		memset(buf, 0xff, size);
		break;
	case 2:
		for (i = 0; i < size; i += 8) {
			uint64_t v = rng_next();

			memcpy(buf + i, &v, size - i < 8 ? size - i : 8);
		}
		break;
	default:
		for (i = 0; i < size; i++)
			buf[i] = ~buf[i];
		break;
	}
}

/* pwrite or pread all of buf at offset 0; -1 with errno set on failure */
static int xfer_all(int fd, unsigned char *buf, size_t size, int write)
{
	size_t done = 0;
	ssize_t n;

	while (done < size) {
		if (write)
			n = pwrite(fd, buf + done, size - done, done);
		else
			n = pread(fd, buf + done, size - done, done);
		if (n < 0)
			return -1;
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		done += n;
	}

	return 0;
}

static int qualify(const char *path, int passes)
{
	unsigned char *want = NULL, *got = NULL;
	unsigned long long errors = 0;
	long long first = -1;
	int fd, pass, p, ret;
	off_t size;
	size_t i;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		printf("%s error: open: %s\n", path, strerror(errno));
		return DEVICE_ERROR;
	}

	size = lseek(fd, 0, SEEK_END);
	want = malloc(size > 0 ? size : 1);
	got = malloc(size > 0 ? size : 1);
	if (size <= 0 || !want || !got) {
		printf("%s error: no scratch memory to test\n", path);
		ret = DEVICE_ERROR;
		goto out;
	}

	for (pass = 0; pass < passes; pass++) {
		for (p = 0; p < NR_PATTERNS; p++) {
			fill(want, size, p);
			if (xfer_all(fd, want, size, 1) || xfer_all(fd, got, size, 0)) {
				printf("%s error: %s\n", path, strerror(errno));
				ret = DEVICE_ERROR;
				goto out;
			}

			for (i = 0; i < (size_t)size; i++) {
				unsigned char diff = want[i] ^ got[i];

				if (!diff)
					continue;
				errors += __builtin_popcount(diff);
				if (first < 0)
					first = i;
			}
		}
	}

	if (errors) {
		printf("%s fail: %llu bit errors, first at byte %lld\n",
			path, errors, first);
		ret = MISMATCH;
	} else {
		printf("%s pass\n", path);
		ret = PASS;
	}
out:
	free(want);
	free(got);
	close(fd);
	return ret;
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-n passes] [-s seed] /dev/pyriteN...\n", prog);
	exit(USAGE);
}

int main(int argc, char **argv)
{
	unsigned long long seed = time(NULL) ^ getpid();
	int passes = 1, worst = PASS, opt, i, r;

	while ((opt = getopt(argc, argv, "n:s:")) != -1) {
		switch (opt) {
		case 'n':
			passes = atoi(optarg);
			break;
		case 's':
			seed = strtoull(optarg, NULL, 0);
			break;
		default:
			usage(argv[0]);
		}
	}
	if (optind == argc || passes < 1)
		usage(argv[0]);

	rng = seed ? seed : 1;

	for (i = optind; i < argc; i++) {
		r = qualify(argv[i], passes);
		if (r > worst)
			worst = r;
	}

	return worst;
}
