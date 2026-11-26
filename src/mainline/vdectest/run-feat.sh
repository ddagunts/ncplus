#!/bin/sh
# run-feat.sh [names...]: decode each clip of feat.list (name width height) with v4l2slh264dec and
# compare one md5 per NV12 frame with ffmpeg's (name.md5). Prints one line per clip.
G=/root/gst
export GST_PLUGIN_PATH=$G/plugins LD_LIBRARY_PATH=$G/lib GST_REGISTRY=$G/registry.bin
cd "$(dirname "$0")"
while read -r n w h; do
	[ $# -gt 0 ] && ! echo " $* " | grep -q " $n " && continue
	fs=$((w * h * 3 / 2)); nf=$(wc -l < $n.md5)
	rm -f /tmp/v.fifo; mkfifo /tmp/v.fifo
	( i=0; while [ $i -lt $nf ]; do dd bs=$fs count=1 iflag=fullblock 2>/dev/null | md5sum | cut -d" " -f1; i=$((i+1)); done; cat > /dev/null ) < /tmp/v.fifo > $n.got &
	t0=$(date +%s%N)
	$G/bin/gst-launch-1.0 -q filesrc location=$n.h264 ! h264parse ! v4l2slh264dec ! video/x-raw,format=NV12 ! filesink location=/tmp/v.fifo > $n.log 2>&1
	rc=$?; wait; t1=$(date +%s%N)
	ok=$(paste -d" " $n.got $n.md5 | awk '$1==$2' | wc -l)
	bad=$(paste -d" " $n.got $n.md5 | awk '$1!=$2{print NR}' | head -8 | paste -sd,)
	echo "$n ${w}x$h: rc $rc, $ok/$nf frames identical, $(( (t1 - t0) / 1000000 )) ms${bad:+, first differing: $bad}"
done < feat.list
