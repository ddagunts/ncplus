#!/bin/sh
# vplay.sh [clip] [times]: play an H.264 clip full screen on the TV's video plane: mt5398-vdec
# decodes, mt5398-vplane shows (as root; GStreamer pieces unpacked in /root/gst: gst-launch,
# v4l2codecs, h264parse, video4linux2 (v4l2sink), isomp4, with libv4l2 in /root/gst/lib).
# Raw .h264 or .mp4/.mov (qtdemux), by extension; video only, no audio yet.
# The decoder's NV12 buffers go to the video plane as DMABUFs: no copy between the two.
G=/root/gst; export GST_PLUGIN_PATH=$G/plugins LD_LIBRARY_PATH=$G/lib GST_REGISTRY=$G/registry.bin
c=${1:-/root/vdec/doom.h264}; n=${2:-1}
dev=
for d in /sys/class/video4linux/video*; do
	[ "$(cat $d/name 2>/dev/null)" = mt5398-vplane ] && dev=/dev/${d##*/}
done
[ -n "$dev" ] || { echo "no mt5398-vplane device (modprobe mt5398-vplane)" >&2; exit 1; }
case "$c" in
*.mp4|*.mov|*.m4v) demux="qtdemux name=d d.video_0 !" ;;
*) demux= ;;
esac
while [ $n -gt 0 ]; do
	$G/bin/gst-launch-1.0 filesrc location="$c" ! $demux h264parse ! v4l2slh264dec ! \
		v4l2sink device=$dev io-mode=dmabuf-import sync=true
	n=$((n - 1))
done
