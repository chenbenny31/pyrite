// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../uapi/pyrite.h"

int main(int argc, char **argv)
{
	struct pyrite_alloc_req req = { 0 };
	int fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s /dev/pyriteN bytes\n", argv[0]);
		return 1;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	req.bytes = strtoull(argv[2], NULL, 0);
	if (ioctl(fd, PYRITE_IOC_ALLOC, &req) < 0) {
		perror("ioctl ALLOC");
		return 1;
	}

	printf("alloc_id=%llu rounded=%llu pid=%d\n",
		(unsigned long long)req.alloc_id,
		(unsigned long long)req.bytes, getpid());
	fflush(stdout);

	pause();
	return 0;
}
