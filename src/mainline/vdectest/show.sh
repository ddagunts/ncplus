#!/bin/sh
# show.sh [clip] [times]: play an H.264 elementary stream in a window on the Debian desktop
# through mt5398-vdec (as root; GStreamer pieces unpacked in /root/gst: gst-launch,
# v4l2codecs, h264parse, videoconvertscale, ximagesink). About 18 fps at 720p: the CPU
# colour conversion and X are the limit, not the decoder.
G=/root/gst; export GST_PLUGIN_PATH=$G/plugins LD_LIBRARY_PATH=$G/lib GST_REGISTRY=$G/registry.bin DISPLAY=:0 XAUTHORITY=/home/tv/.Xauthority
c=${1:-/root/vdec/clip.h264}; n=${2:-3}
while [ $n -gt 0 ]; do
	$G/bin/gst-launch-1.0 -q filesrc location="$c" ! h264parse ! v4l2slh264dec ! queue ! videoconvert chroma-mode=none dither=none n-threads=2 ! queue ! ximagesink sync=true
	n=$((n - 1))
done
