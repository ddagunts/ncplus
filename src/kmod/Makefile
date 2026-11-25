# Out-of-tree modules for the TV's stock kernel (3.0.13 SMP preempt mod_unload ARMv7).
# KDIR = LG's own MT5398 GPL tree, prepared by third_party/build-deps.sh kmod.
KDIR  ?= $(CURDIR)/../../third_party/kernel/linux-3.0.13-lg
CROSS ?= $(CURDIR)/../../third_party/kernel/armv7-eabihf--glibc--stable-2018.11-1/bin/arm-linux-
# CC on the command line: the MTK tree's Makefile uses `CC ?=`, so make's default cc would win.
# The MTK tree serializes builds with procmail's `lockfile`; build/kbin has a shim.
export PATH := $(CURDIR)/../../build/kbin:$(PATH)
MK = $(MAKE) -C $(KDIR) ARCH=arm CROSS_COMPILE=$(CROSS) CC=$(CROSS)gcc
all:
	$(MK) KCFLAGS="-fno-pie -fno-pic -std=gnu89 -Wno-error" M=$(CURDIR) modules
clean:
	$(MK) M=$(CURDIR) clean
