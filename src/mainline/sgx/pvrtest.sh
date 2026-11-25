#!/bin/sh
# SGX544 bring-up under Debian (netcast-pvr.service, opt-in: <stick>/netcast/mainline/pvrtest).
# Lives on the stick as <stick>/netcast/mainline/pvr/pvrtest.sh, so it changes without a new
# debian.img. Everything goes to the kernel log (ramoops: tools/ramlog.py from stock) and to
# <stick>/netcast/mainline/pvr/pvrtest.log.
#
# <stick>/netcast/mainline/pvr/
#   pvrsrvkm.ko     our services kernel module (third_party/build-sgx.sh)
#   sgxgltest       the off-screen GLES2 test (src/mainline/sgx/test)
#   libpvrEWS_NullWSEGL.so  our WSEGL (src/mainline/sgx/wsegl)
#   debs/           Debian armel libc6 + libgcc-s1, for images without armel multiarch
#   lib/            the TV's own DDK 1.9 userspace, copied from /mnt/lg/res/lglib by fire.sh
# Steps: unbind mt5398-sgx, insmod pvrsrvkm, pvrsrvinit (loads the microkernel), sgxgltest.
# The watchdog is armed: a GPU or bus hang resets the TV to stock.
M=/mnt/stick/netcast/mainline
P=$M/pvr
L=/run/pvr
exec >> "$P/pvrtest.log" 2>&1
log() { echo "pvrtest: $*"; echo "pvrtest: $*" > /dev/kmsg; }
fail() { log "FAIL: $*"; sync; exit 1; }

log "start $(date) kernel $(uname -r)"
[ -f "$P/pvrsrvkm.ko" ] || fail "no $P/pvrsrvkm.ko"
[ -s "$P/lib/libsrv_um.so" ] || fail "no $P/lib (run fire.sh under the stock system once)"
if [ ! -e /lib/ld-linux.so.3 ]; then
	# images built before armel multiarch: unpack Debian's armel libc6 + libgcc-s1 from
	# <stick>/netcast/mainline/pvr/debs (their files don't clash with armhf's)
	for d in "$P"/debs/*_armel.deb; do
		[ -f "$d" ] && dpkg-deb -x "$d" / && log "unpacked $(basename "$d")"
	done
	ldconfig
fi
[ -e /lib/ld-linux.so.3 ] || fail "no armel loader (libc6:armel)"

if [ -e /sys/bus/platform/drivers/mt5398-sgx/f0040000.gpu ]; then
	echo f0040000.gpu > /sys/bus/platform/drivers/mt5398-sgx/unbind && log "unbound mt5398-sgx"
fi
grep -q '^pvrsrvkm ' /proc/modules || insmod "$P/pvrsrvkm.ko" || fail "insmod"
sleep 1
dmesg | grep -i -E 'pvr|sgx' | tail -20 | while read -r l; do log "dmesg: $l"; done
[ -c /dev/pvrsrvkm ] || fail "no /dev/pvrsrvkm"

# LG's libraries under the names IMGegl dlopens; NullWSEGL: no window system, pbuffers only
mkdir -p "$L"
cp "$P"/lib/* "$L/"
ln -sf libEGL.so.1.4 "$L/libEGL.so"
ln -sf libGLESv2.so.2.0 "$L/libGLESv2.so"
ln -sf libGLESv1_CM.so.1.1 "$L/libGLESv1_CM.so"
chmod +x "$L/pvrsrvinit"
# our window system module (src/mainline/sgx/wsegl, pbuffers only for now) under the name
# LG's IMGegl falls back to when its NetCast WSEGL can't load (no libUMA.so here)
cp "$P/libpvrEWS_NullWSEGL.so" "$L/" || fail "no $P/libpvrEWS_NullWSEGL.so"

log "pvrsrvinit"
LD_LIBRARY_PATH=$L "$L/pvrsrvinit"
log "pvrsrvinit exit $?"
dmesg | grep -i -E 'pvr|sgx' | tail -20 | while read -r l; do log "dmesg: $l"; done

if [ -f "$P/sgxgltest" ]; then
	# run from tmpfs: exec permission on the FAT stick depends on its mount options
	cp "$P/sgxgltest" "$L/" && chmod +x "$L/sgxgltest"
	log "sgxgltest"
	LD_LIBRARY_PATH=$L timeout 120 "$L/sgxgltest" -o "$P/frame.ppm" 2>&1 | while read -r l; do log "$l"; done
fi
dmesg | grep -i -E 'pvr|sgx' | tail -20 | while read -r l; do log "dmesg: $l"; done
log "done"
sync
