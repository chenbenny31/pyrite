# SPDX-License-Identifier: GPL-2.0-only

KDIR ?= $(CURDIR)/linux
PWD  := $(CURDIR)

.PHONY: all clean check

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Not kbuild's "M= clean": that runs a recursive find under M and would
# delete the build artifacts of a kernel tree kept in ./linux
clean:
	rm -f *.o *.ko *.mod *.mod.c .*.cmd .*.d modules.order Module.symvers

check:
	$(KDIR)/scripts/checkpatch.pl --no-tree -f pyrite.c uapi/pyrite.h
