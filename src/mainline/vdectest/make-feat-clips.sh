#!/bin/bash
# make-feat-clips.sh <source video> <outdir>: H.264 feature test clips for mt5398-vdec, each with
# one md5 per ffmpeg-decoded NV12 frame (name.md5) and feat.list (name width height) for
# run-feat.sh on the TV. Needs ffmpeg with libx264.
set -euo pipefail
SRC=$1 OUT=$2; mkdir -p "$OUT"; cd "$OUT"; : > feat.list
enc() {	# enc name frames vf ffmpeg-args...
	local n=$1 f=$2 vf=$3; shift 3
	ffmpeg -v error -y -i "$SRC" -frames:v "$f" -vf "$vf" -an -c:v libx264 -pix_fmt yuv420p "$@" -f h264 "$n.h264"
	ffmpeg -v error -y -i "$n.h264" -pix_fmt nv12 -f rawvideo "$n.nv12"
	read -r w h < <(ffprobe -v error -select_streams v -show_entries stream=width,height -of csv=p=0 "$n.h264" | tr , ' ')
	python3 -c "
import hashlib; fs=$w*$h*3//2; d=open('$n.nv12','rb').read()
open('$n.md5','w').write(''.join(hashlib.md5(d[i:i+fs]).hexdigest()+'\n' for i in range(0,len(d),fs)))"
	rm "$n.nv12"; echo "$n $w $h" >> feat.list
}
enc t1080     60 scale=1920:1080:flags=lanczos -preset medium -crf 20
enc tslices   60 null -preset medium -crf 23 -x264-params slices=4
enc tcavlc    60 null -profile:v main -coder 0 -bf 3 -crf 23
enc tbase848  60 scale=848:480 -profile:v baseline -crf 23
enc tcqm      60 null -preset medium -crf 23 -x264-params cqm=jvt
enc ttemporal 60 null -preset medium -crf 23 -x264-params direct=temporal:bframes=3:b-pyramid=none
enc tdeblock  60 null -preset fast -crf 23 -x264-params deblock=-3,2:weightp=0:ref=1
enc tnodeblk  60 null -preset fast -crf 23 -x264-params no-deblock=1
enc tcif      60 scale=352:288 -preset slow -crf 23 -x264-params ref=6:bframes=5:b-adapt=2
enc tgop      60 null -preset fast -crf 23 -x264-params keyint=20:open-gop=1:bframes=3
enc tslqp     10 null -qp 26 -x264-params slices=4
enc tslcavlc  10 null -profile:v main -coder 0 -qp 26 -x264-params slices=4
enc tsl2i     10 null -qp 26 -x264-params slices=2:keyint=1
enc tslnodb   10 null -qp 26 -x264-params slices=4:no-deblock=1
enc tsl2      10 null -qp 26 -x264-params slices=2:keyint=1:bframes=0
cat feat.list
