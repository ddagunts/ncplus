#!/bin/sh
# Display test for the mt5398_osd KMS driver, run under Debian on the TV by
# netcast-gfxtest.service when <stick>/netcast/mainline/gfxtest exists (this file sits next
# to it as gfxtest.sh). Everything goes to the kernel log, so it lands in ramoops; read it
# back from stock with tools/ramlog.py.
#   1. modetest: static SMPTE bars in ARGB8888, then XRGB8888 (8 s each). In ARGB the
#      middle row is half transparent (libdrm alpha 127); everything else is opaque.
#   2. weston (DRM backend, pixman renderer) for 40 s with demo clients
# flip_stats = "<latched> <timeout> blend <plane +8> hdr ...": flip events completed because
# the OSD fetched the new header / because the driver gave up waiting (3 ticks).
log() { while IFS= read -r l; do echo "gfxtest: $l" > /dev/kmsg; done; }
say() { echo "$*" | log; }
ST=/sys/bus/platform/devices/f0021000.display/flip_stats
stats() { say "flip_stats after $1: $(cat "$ST" 2>/dev/null || echo 'n/a')"; }
D=mt5398-osd

say "=== display test"
stats start

# Static SMPTE bars (no -v): modetest -v flips between the pattern and a plain second
# buffer every vblank, which flickers and looks translucent on camera.
for f in AR24 XR24; do
	say "--- modetest static $f, 8 s"
	sleep 8 | timeout 20 modetest -M $D -s LVDS-1:1920x1080@$f 2>&1 |
		tr '\r' '\n' | grep -v '^[[:space:]]*$' | tail -n 4 | log
	stats "static $f"
done

say "--- weston (drm, pixman), 40 s"
R=/run/gfxtest
mkdir -p -m 700 $R
# Debian's seatd.service is already running: use it (seatd-launch would refuse)
LIBSEAT_BACKEND=seatd XDG_RUNTIME_DIR=$R weston --backend=drm --renderer=pixman \
	--continue-without-input --socket=wayland-gfx --log=$R/weston.log >$R/weston.out 2>&1 &
W=$!
n=0
while [ ! -S $R/wayland-gfx ] && [ $n -lt 20 ]; do sleep 1; n=$((n + 1)); done
if [ -S $R/wayland-gfx ]; then
	say "weston up after ${n}s"
	for c in weston-flower weston-simple-shm weston-terminal; do
		command -v $c >/dev/null || continue
		XDG_RUNTIME_DIR=$R WAYLAND_DISPLAY=wayland-gfx $c >$R/$c.out 2>&1 &
		say "client: $c"
		sleep 1
	done
	sleep 40
	stats weston
else
	say "weston: no socket after ${n}s"
fi
kill $W 2>/dev/null
sleep 2
pkill -f 'weston-(flower|simple-shm|terminal)' 2>/dev/null
cat $R/weston.out $R/weston.log 2>/dev/null | grep -v '^[[:space:]]*$' | head -n 80 | log
for f in $R/weston-*.out; do [ -s "$f" ] && { say "${f##*/}:"; head -n 10 "$f" | log; }; done
stats end
say "=== display test done"
