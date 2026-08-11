# Out-of-tree Makefile for the owgw-spi-master kernel module.
#
# Usage:
#   make                        # build the .ko
#   sudo make install           # install to /lib/modules/$(uname -r)/extra/
#   make clean                  # remove build artifacts
#
# Override KDIR if your kernel sources are not at the running kernel's
# build symlink, e.g.:
#   make KDIR=/path/to/linux-6.6.30

obj-m := owgw-spi-master.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

CC ?= $(CROSS_COMPILE)gcc

# Required: <linux/iio/temperature/owgw_iface.h> resolves from repo root.
ccflags-y := -I$(src)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

install: modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all install modules_install clean
