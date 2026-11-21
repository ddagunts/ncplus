#!/bin/sh
# Warm-boot the stock LG kernel through nckexec. Put it next to nckexec.ko and kernA.uimg
# (tools/unlz4p.py p21.bin kernA.uimg) on the stick, then run it detached:
#   nohup sh fire.sh >/dev/null 2>&1 &
# Success shows up as a fresh boot whose /proc/cmdline ends in "maxcpus=1 panic=5".
K=$(cd "$(dirname "$0")" && pwd)
sync
echo 0 > /sys/devices/system/cpu/cpu1/online || exit 1
if ! insmod "$K/nckexec.ko" image="$K/kernA.uimg" 'append=maxcpus=1;panic=5'; then
	echo 1 > /sys/devices/system/cpu/cpu1/online
	exit 1
fi
dmesg | grep nckexec > "$K/fire.log"
sync
sleep 1
echo 1 > /sys/module/nckexec/parameters/go
