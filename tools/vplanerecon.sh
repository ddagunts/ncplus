#!/bin/bash
# vplanerecon.sh <label> [stock|debian] -- read-only snapshot of the MT5398 video path, for the
# mainline video plane (B2R -> scaler -> mixer). Writes nothing on the TV.
#
# Blocks (from LG's dtv_driver_adpt.ko): CKGEN 0xf000d000, OSD 0xf0021000, 0xf0022000..0xf0025000
# (video in / PQ / deinterlacer / scaler), B2R 0xf0026000, 0xf0027000 (mixer, post), 0xf005e000
# (output timing). B2R's clock is off while no video plays (CKGEN+0x228 bit 0, set by LG's player
# along with the clock divider at +0x22c): its page, and the decoder's MC page, are only read
# when it is on.
# B2R is read three times 50 ms apart, so per-frame address flips show up.
#
#   stock:  TV on LG's firmware, telnet up; memscan from <stick>/netcast/tools/
#   debian: root ssh on port 2222 to $NETCAST_SSH (e.g. root@<TV address>), /root/memscan
# EXTRA="0xf0036000 ..." adds pages (DRAM controller 0xf0007000, MJC 0xf0036000, LVDS 0xf0034000...).
# Output: tv_dump/vplane/<label>-<target>/ (gitignored: LG register state).
set -e
cd "$(dirname "$0")/.."
LABEL=${1:?label}
TARGET=${2:-stock}
OUT=tv_dump/vplane/$LABEL-$TARGET
mkdir -p "$OUT"

if [ "$TARGET" = stock ]; then
  T() { python3 -I tools/tv.py "$1" 60 | tr -d '\r'; }
  root=$(T 'ls -d /mnt/usb/usb*/Drive*/netcast 2>/dev/null | head -1' | tail -1)
  [ -n "$root" ] || { echo "no stick found on the TV" >&2; exit 1; }
  M="$root/tools/memscan"	# the release keeps it in tools/; older sticks in mainline/
  T "[ -x $M ] || echo missing" | grep -q missing && M="$root/mainline/memscan"
else
  T() { ssh -p 2222 -o ConnectTimeout=8 "${NETCAST_SSH:?set NETCAST_SSH=root@<TV address>}" "$1"; }
  M=/root/memscan
fi

dump() { T "$M -d $1 ${2:-0x1000}" | grep -E '^[0-9a-f]{8}:' > "$OUT/$1.txt"; }

dump 0xf000d000
gate=$(awk '$1=="f000d220:"{print $10}' "$OUT/0xf000d000.txt")
echo "CKGEN+228 low byte: $gate"
for b in 0xf0021000 0xf0022000 0xf0023000 0xf0024000 0xf0025000 0xf0027000 0xf005e000 $EXTRA; do
  dump $b
done
if [ $((0x$gate & 1)) -ne 0 ]; then
  echo "B2R clocked: reading B2R and MC"
  dump 0xf0026000
  T "for i in 1 2 3; do $M -d 0xf0026000 0x400; usleep 50000 2>/dev/null || sleep 0.05; done" \
    | grep -E '^[0-9a-f]{8}:' > "$OUT/b2r-x3.txt"
  dump 0xf000a000
else
  echo "B2R gated: not read"
fi
ls "$OUT"
