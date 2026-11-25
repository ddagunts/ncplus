#!/bin/sh
# Warm-boot a kernel through nckexec. Put it next to nckexec.ko and kernA.uimg
# (tools/unlz4p.py p21.bin kernA.uimg) on the stick, then run it detached:
#   nohup sh fire.sh >/dev/null 2>&1 &
# Success shows up as a fresh boot whose /proc/cmdline ends in "maxcpus=1 panic=5".
#
# WARNING: never boot a different (non-stock) kernel on a TV that doesn't pass ncpreflight.
# A kernel that misbehaves (corrupts flash, leaves the hardware in a state the next boot trips
# over) can leave it permanently unbootable. This script refuses unless ncpreflight says ok.
K=$(cd "$(dirname "$0")" && pwd)
PREFLIGHT=
for d in "$K" "$K/../tools" "$K/../netcast/tools"; do [ -x "$d/ncpreflight" ] && PREFLIGHT=$d/ncpreflight && break; done
[ -n "$PREFLIGHT" ] || { echo "fire.sh: ncpreflight not found next to this script or in netcast/tools: refusing" >&2; exit 1; }
if ! RESULT=$("$PREFLIGHT" 2>&1) || [ "$RESULT" != "preflight: ok" ]; then
	echo "fire.sh: ${RESULT:-preflight failed}: refusing to boot another kernel" >&2
	exit 1
fi
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
