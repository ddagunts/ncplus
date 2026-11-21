#!/bin/sh
# Warm-boot a kernel through nckexec. Put it next to nckexec.ko and kernA.uimg
# (tools/unlz4p.py p21.bin kernA.uimg) on the stick, then run it detached:
#   nohup sh fire.sh >/dev/null 2>&1 &
# Success shows up as a fresh boot whose /proc/cmdline ends in "maxcpus=1 panic=5".
#
# WARNING: never boot a different (non-stock) kernel unless the TV's debug level is DEBUG.
# In RELEASE (most retail TVs) a kernel that misbehaves (corrupts flash, leaves the hardware in a
# state the next signed boot trips over) can end in U-Boot's signature-failure path: a hang plus an
# eFuse burn, with no UART break-in to recover. That is a permanently bricked TV. See FINDINGS.md
# ("Debug level", "Flash, boot chain and secure boot"). This script refuses unless ncdebug reports DEBUG.
K=$(cd "$(dirname "$0")" && pwd)
NCDEBUG=
for d in "$K" "$K/../tools" "$K/../netcast/tools"; do [ -x "$d/ncdebug" ] && NCDEBUG=$d/ncdebug && break; done
[ -n "$NCDEBUG" ] || { echo "fire.sh: ncdebug not found next to this script or in netcast/tools: refusing" >&2; exit 1; }
LEVEL=$("$NCDEBUG" status 2>/dev/null | sed -n 's/^debug level @0x18a: [0-9]* = //p')
if [ "$LEVEL" != DEBUG ]; then
	echo "fire.sh: debug level is '${LEVEL:-unknown}', not DEBUG: refusing to boot another kernel" >&2
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
