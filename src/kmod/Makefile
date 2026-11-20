# Out-of-tree modules for the TV's stock kernel (3.0.13 SMP preempt mod_unload ARMv7).
KDIR  ?= $(CURDIR)/../../third_party/kernel/linux-3.0.13
CROSS ?= $(CURDIR)/../../third_party/kernel/armv7-eabihf--glibc--stable-2018.11-1/bin/arm-linux-
all:
	$(MAKE) -C $(KDIR) ARCH=arm CROSS_COMPILE=$(CROSS) KCFLAGS="-fno-pie -fno-pic" M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) ARCH=arm CROSS_COMPILE=$(CROSS) M=$(CURDIR) clean
