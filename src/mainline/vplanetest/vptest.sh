#!/bin/sh
# vptest.sh <dir> [secs] -- first MT5398 video-plane experiment, on Debian (root).
# <dir> holds vplane, the lists from mklist.py and frame.tiled (mkframe.py).
#   1. frame.tiled -> 0x2c000000 (RAM hole, outside Linux's memory)
#   2. 1-b2r.list, 3-on.list, 2-path.list, 4-trig.list: B2R set up and running first, so the path
#      sees a live input while it is switched to LG's 720p playback state, then the import
#      triggers latch it; the old values go to <list>.undo
#   3. B2R status: the line counter (+0x3c bits 16-27) must move, the vsync bits (+0x30c) set
#   4. OSD plane 3 off for [secs] (default 15), so only the video plane is on the panel, then on
# Undo the path: for l in 4-trig 2-path 3-on 1-b2r; do vplane w $l.list.undo; done
set -e
cd "$1"
S=${2:-15}
V=./vplane
$V load 2c000000 frame.tiled
for l in 1-b2r 3-on 2-path 4-trig; do
  $V w $l.list > $l.list.undo
  echo "$l: $(wc -l < $l.list.undo) words written"
done
for i in 1 2 3; do $V r f002603c; sleep 0.02; done
$V r f002630c
echo "OSD plane 3 off for $S s"
$V w - <<EOF > osd.undo
f0021300 0 1
f0021000 1 1
f0021000 8 8
EOF
sleep "$S"
$V w - <<EOF > /dev/null
f0021300 1 1
f0021000 1 1
f0021000 8 8
EOF
echo "OSD back on"
