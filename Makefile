# SPDX-License-Identifier: GPL-2.0-only

KDIR ?= $(HOME)/src/linux
PWD  := $(CURDIR)

.PHONY: all clean check

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

check:
	$(KDIR)/scripts/checkpath.pl --no-tree -f pyrite.c uapi/pyrite.h
