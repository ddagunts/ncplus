#!/bin/sh
# Warm-boot the mainline kernel through nckexec. Lives in <stick>/netcast/mainline/ next to
# nckexec.ko, Image and mt5398-lg-netcast.dtb (third_party/build-mainline.sh); ncpreflight is
# found in ../tools.
#
#   sh fire.sh probe                          # stage + OSD dry run, nothing is booted
#   nohup sh fire.sh go >/dev/null 2>&1 &     # boot it (irreversible until power-cycle)
#
# "probe" loads nckexec (which reads the Image and DTB into staging pages), asks it to decode
# OSD plane 3 exactly as the real fire would, logs the result to fire.log and unloads again.
# "go" does the same staging, then hands over. If nckexec refuses (OSD not in the expected
# state, CPU1 offline, ...) the stock system keeps running and fire.log says why.
#
# nckexec takes the panel over from LG's compositor, whose OSD is only in a known state while
# an app window is up (on plane 3 or, seen on other screens than HOME, plane 2). If none is
# (nothing loaded, e.g. after a return from mainline), both modes first start ncboot, a "Starting Linux" splash app (run through
# run-app.sh, found next to this script or in ../tools or ../apps), and wait until it shows.
#
# The panel keeps the refresh rate it has at the jump. "go" first sets it to 60 Hz with ncrate
# (found next to this script or in ../tools); NETCAST_HZ=50 or NETCAST_HZ=keep in the
# environment choose otherwise. If the rate can't be set the boot goes on at the rate there is
# (fire.log says so); if nckexec then refuses, the old rate is put back. "probe" only
# reports the rate.
#
# WARNING: never boot a different (non-stock) kernel on a TV that doesn't pass ncpreflight.
# A kernel that misbehaves can leave it permanently unbootable. This script refuses unless
# ncpreflight says ok; don't bypass it.
K=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-}
case "$MODE" in probe|go) ;; *) echo "usage: fire.sh probe|go" >&2; exit 2;; esac
HZ=${NETCAST_HZ:-60}
case "$HZ" in 60|50|keep) ;; *) echo "fire.sh: NETCAST_HZ must be 60, 50 or keep" >&2; exit 2;; esac
for f in nckexec.ko Image mt5398-lg-netcast.dtb; do
	[ -s "$K/$f" ] || { echo "fire.sh: missing $K/$f" >&2; exit 1; }
done
NCRATE=
for d in "$K" "$K/../tools"; do [ -x "$d/ncrate" ] && NCRATE=$d/ncrate && break; done
PREFLIGHT=
for d in "$K" "$K/../tools"; do [ -x "$d/ncpreflight" ] && PREFLIGHT=$d/ncpreflight && break; done
[ -n "$PREFLIGHT" ] || { echo "fire.sh: ncpreflight not found: refusing" >&2; exit 1; }
if ! RESULT=$("$PREFLIGHT" 2>&1) || [ "$RESULT" != "preflight: ok" ]; then
	echo "fire.sh: ${RESULT:-preflight failed}: refusing to boot another kernel" >&2
	exit 1
fi

# The TV's own OpenGL ES userspace (PowerVR DDK 1.9, LG's build) for the mainline SGX driver:
# copied from this TV's read-only lglib onto the stick once, never shipped by us.
PVRLIB="$K/pvr/lib"
if [ ! -s "$PVRLIB/libsrv_um.so" ] && [ -d /mnt/lg/res/lglib ]; then
	mkdir -p "$PVRLIB"
	for f in pvrsrvinit libsrv_um.so libsrv_init.so libIMGegl.so libEGL.so.1.4 \
		libGLESv2.so.2.0 libGLESv1_CM.so.1.1 libglslcompiler.so libusc.so libpvr2d.so \
		libpvrPVR2D_NullWSEGL.so libPVRScopeServices.so; do
		[ -f "/mnt/lg/res/lglib/$f" ] && cp "/mnt/lg/res/lglib/$f" "$PVRLIB/"
	done
	sync
fi

SPLASH=
FORCED=
RATELOG=/tmp/ncrate.log
rm -f $RATELOG
# what nckexec and ncrate said; nckexec's lines go to the kernel log
save_log() {
	dmesg | grep nckexec > "$K/fire.log"
	[ -f $RATELOG ] && cat $RATELOG >> "$K/fire.log"
}
undo() {
	[ -n "$FORCED" ] && "$NCRATE" release "$FORCED" >> $RATELOG 2>&1
	[ -n "$SPLASH" ] && kill "$SPLASH" 2>/dev/null	# SIGTERM: libnc unregisters cleanly
	save_log
	rmmod nckexec 2>/dev/null
	echo 1 > /sys/devices/system/cpu/cpu1/online
	sync
}

grep -q nckexec /proc/modules && rmmod nckexec
sync
# CPU1 stays online: the DTB's nckexec,cpu1-hold has nckexec park it for the mainline SMP
# enable-method (an older DTB without it makes nckexec refuse until CPU1 is offline).
echo 1 > /sys/devices/system/cpu/cpu1/online 2>/dev/null
if ! insmod "$K/nckexec.ko" image="$K/Image" dtb="$K/mt5398-lg-netcast.dtb"; then
	undo
	exit 1
fi
GO=/sys/module/nckexec/parameters/go
READY=/tmp/ncboot.ready

# put ncboot up and wait until it has drawn
start_splash() {
	rm -f $READY
	sh "$RUNAPP" "$NCBOOT" $READY 120 > /tmp/ncboot.log 2>&1 &
	SPLASH=$!	# run-app.sh execs the app: this is its pid
	n=0
	while [ ! -e $READY ]; do
		n=$((n + 1))
		if [ $n -gt 30 ] || ! kill -0 "$SPLASH" 2>/dev/null; then
			echo "fire.sh: ncboot splash did not come up:" >&2
			tail -5 /tmp/ncboot.log >&2
			return 1
		fi
		sleep 1
	done
}

# wait_layer: up to 10 s for LG's 1280x720 layer to show on plane 3 or 2
wait_layer() {
	n=0
	until echo layer > $GO 2>/dev/null; do
		n=$((n + 1))
		[ $n -ge 10 ] && return 1
		sleep 1
	done
}

# set_rate: panel to $HZ, after the splash is up (LG may change the rate with the screen).
# Fails only if the panel must not be handed over now (OSD left faded out: a black screen).
set_rate() {
	if [ -z "$NCRATE" ]; then
		echo "fire.sh: no ncrate: the panel keeps its rate" > $RATELOG
		return 0
	fi
	# what LG's software had, for undo
	FORCED=$("$NCRATE" status 2>/dev/null | sed -n "s/.*LG's driver says \([0-9]*\) Hz.*/\1/p")
	"$NCRATE" "$HZ" > $RATELOG 2>&1
	r=$?
	cat $RATELOG
	[ $r = 2 ] && FORCED=		# it didn't touch anything
	[ $r != 3 ]
}

give_up() {
	echo "fire.sh: $1" >&2
	echo dump > $GO 2>/dev/null		# while the splash is still up
	undo
	tail -8 "$K/fire.log" >&2
	exit 1
}

if ! echo layer > $GO 2>/dev/null; then
	NCBOOT= RUNAPP=
	for d in "$K" "$K/../tools" "$K/../apps"; do
		[ -z "$NCBOOT" ] && [ -x "$d/ncboot" ] && NCBOOT=$d/ncboot
		[ -z "$RUNAPP" ] && [ -f "$d/run-app.sh" ] && RUNAPP=$d/run-app.sh
	done
	if [ -z "$NCBOOT" ] || [ -z "$RUNAPP" ]; then
		echo "fire.sh: no app on screen and no ncboot/run-app.sh to put one up" >&2
		undo
		exit 1
	fi
	echo dump > $GO 2>/dev/null		# the screen fire.sh was started from
	start_splash || give_up "no splash"
	wait_layer || give_up "ncboot is up but LG's layer is on neither plane 3 nor plane 2:"
fi
if [ "$MODE" = probe ]; then
	echo probe > $GO
	[ -n "$NCRATE" ] && "$NCRATE" status > $RATELOG 2>&1
	undo
	cat "$K/fire.log"
	exit 0
fi
if [ "$HZ" != keep ]; then
	set_rate || give_up "the rate change left the OSD faded out"
	wait_layer || give_up "LG's layer is gone after the rate change:"
fi
save_log
sync
sleep 1
echo 1 > $GO
# Only reached if nckexec refused before shutting anything down.
undo
exit 1
