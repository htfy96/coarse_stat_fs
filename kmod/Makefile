# SPDX-License-Identifier: GPL-2.0
KDIR ?= /lib/modules/$(shell uname -r)/build

ifeq ($(wildcard $(KDIR)/Makefile),)
$(error no kernel headers at $(KDIR): install them (linux-headers on Arch, linux-headers-$(shell uname -r) on Debian) or pass KDIR=<path>)
endif

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
