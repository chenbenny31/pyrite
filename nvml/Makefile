# SPDX-License-Identifier: GPL-2.0-only
# The NVML stand-in. It shadows the real libnvidia-ml.so.1, so never install it
# on a machine with an NVIDIA driver; vm/vm.sh provision installs it in the VMs.

CFLAGS ?= -O2 -Wall -Wextra
LIBDIR ?= /usr/local/lib64

.PHONY: all check install clean

all: libnvidia-ml.so.1

libnvidia-ml.so.1: nvml.c ../uapi/pyrite.h
	$(CC) $(CFLAGS) -shared -fPIC -pthread -Wl,-soname,$@ -o $@ nvml.c

# Only NVML's names leave the library
check: libnvidia-ml.so.1
	! nm -D --defined-only $< | grep -v ' nvml'

install: libnvidia-ml.so.1
	install -D -m 0755 $< $(DESTDIR)$(LIBDIR)/$<

clean:
	rm -f libnvidia-ml.so.1
