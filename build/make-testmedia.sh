#!/bin/bash
# make-testmedia.sh <outdir>: synthetic test media for the NetCast+ test folder (netcast/test/ on the stick).
# Everything is generated (test patterns, tones), so there is nothing to license. Needs the host's ffmpeg
# with libx264, libx265, libvpx, libopus and libmp3lame. Files that already exist are kept.
set -euo pipefail
O=${1:?usage: make-testmedia.sh <outdir>}
mkdir -p "$O/video" "$O/audio" "$O/photos"
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
FF=(ffmpeg -hide_banner -loglevel error -y)
label() {  # caption + running clock on a test pattern: label <text> <fontsize>
  echo "drawtext=fontfile=$FONT:text='$1':fontsize=$2:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=12:x=(w-tw)/2:y=h*0.12,drawtext=fontfile=$FONT:text='%{pts\\:hms}':fontsize=$2:fontcolor=yellow:box=1:boxcolor=black@0.6:boxborderw=8:x=(w-tw)/2:y=h*0.78"
}
# A/V sync: a short beep at every full second, matching the clock on screen.
BEEP="sine=frequency=880:beep_factor=2:sample_rate=48000"
mk() { local out=$1; shift; [ -s "$out" ] && return 0; echo "  $(basename "$out")"; "${FF[@]}" "$@" "$out"; }

echo "test media -> $O"
# --- video for the hardware Player: H.264 + MP3 in MPEG-TS, the combination verified on the TV ---
# (LG's GStreamer 0.10 path tested 2026-11-20: MP4 fails in qtdemux even with H.264 only; AAC and AC-3 audio
#  and MPEG-2 video have no usable decoder in that path. See FINDINGS.md.)
mk "$O/video/test-h264-mp3-720p.ts" -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "$BEEP" -t 20 \
  -vf "$(label 'H.264 + MP3 720p TS (hardware Player)' 40)" -c:v libx264 -preset slow -crf 28 -pix_fmt yuv420p \
  -profile:v high -level 4.0 -c:a libmp3lame -b:a 128k -f mpegts
# --- software decoding with ffplay (YELLOW in Network Shares): the everyday MP4, MPEG-2, VP9 ---
mk "$O/video/test-h264-aac-480p.mp4" -f lavfi -i "testsrc2=size=854x480:rate=30" -f lavfi -i "$BEEP" -t 15 \
  -vf "$(label 'H.264 + AAC 480p MP4 (ffplay)' 30)" -c:v libx264 -preset slow -crf 28 -pix_fmt yuv420p \
  -profile:v high -level 3.1 -c:a aac -b:a 96k -movflags +faststart -metadata title="NetCast+ test: H.264/AAC MP4"
mk "$O/video/test-mpeg2-ac3-576p.ts" -f lavfi -i "testsrc2=size=720x576:rate=25" -f lavfi -i "$BEEP" -t 15 \
  -vf "$(label 'MPEG-2 + AC-3 576p TS (ffplay)' 32)" -c:v mpeg2video -b:v 2500k -pix_fmt yuv420p \
  -c:a ac3 -b:a 128k -f mpegts
mk "$O/video/test-vp9-opus-360p.mkv" -f lavfi -i "testsrc2=size=640x360:rate=30" -f lavfi -i "$BEEP" -t 15 \
  -vf "$(label 'VP9 + Opus 360p (ffplay)' 24)" -c:v libvpx-vp9 -b:v 600k -deadline good -cpu-used 4 -row-mt 1 \
  -c:a libopus -b:a 64k -metadata title="NetCast+ test: VP9/Opus"
# (no HEVC: on the TV's two A9 cores ffplay dropped over half the frames even at 360p)

# --- photos (also the music cover) and a static listing: BusyBox httpd has no directory index ---
photo() {  # photo <file> <lavfi source> <caption>
  mk "$O/photos/$1" -f lavfi -i "$2" -frames:v 1 -q:v 3 \
    -vf "drawtext=fontfile=$FONT:text='$3':fontsize=48:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=12:x=(w-tw)/2:y=h*0.82"
}
photo smpte-bars-1080p.jpg "smptehdbars=size=1920x1080:rate=1" "SMPTE bars 1080p"
photo test-pattern-720p.jpg "testsrc2=size=1280x720:rate=1" "Test pattern 720p"
photo rgb-test-720p.jpg "rgbtestsrc=size=1280x720:rate=1" "RGB test 720p"
photo cover.jpg "testsrc2=size=500x500:rate=1" "NetCast+"
{
  echo '<!DOCTYPE html><html><head><meta charset="utf-8"><title>NetCast+ test photos</title></head><body><ul>'
  for f in "$O"/photos/*.jpg; do b=$(basename "$f"); echo "<li><a href=\"$b\">$b</a></li>"; done
  echo '</ul></body></html>'
} > "$O/photos/index.html"

# --- music: a 20 s arpeggio over I-IV-V-iii with tags and cover art (FLAC, MP3), and beeps (WAV) ---
K="floor(mod(t*4,4))"; J="floor(mod(t/2,4))"
F="(220*pow(2,(4*$K-gte($K,2)+gte($K,3)+5*gte($J,1)+2*gte($J,2)-4*gte($J,3))/12))"
ENV="exp(-6*mod(t,0.25))*min(1,mod(t,0.25)*300)"
ARP="aevalsrc='(0.25*sin(2*PI*t*$F)+0.08*sin(4*PI*t*$F))*$ENV|(0.25*sin(2*PI*t*$F*1.003)+0.08*sin(4*PI*t*$F))*$ENV':s=44100:d=20"
TAGS=(-metadata artist="NetCast+" -metadata album="Test Tones" -metadata date=2026)
mk "$O/audio/01-arpeggio.flac" -f lavfi -i "$ARP" -i "$O/photos/cover.jpg" -map 0 -map 1 \
  -c:a flac -c:v copy -disposition:v attached_pic "${TAGS[@]}" -metadata title="Arpeggio (FLAC)" -metadata track=1
mk "$O/audio/02-arpeggio.mp3" -f lavfi -i "$ARP" -i "$O/photos/cover.jpg" -map 0 -map 1 \
  -c:a libmp3lame -b:a 192k -c:v copy -id3v2_version 3 -disposition:v attached_pic "${TAGS[@]}" \
  -metadata title="Arpeggio (MP3)" -metadata track=2
mk "$O/audio/03-beeps.wav" -f lavfi -i "sine=frequency=440:beep_factor=2:sample_rate=44100" -t 10 \
  -ac 2 -c:a pcm_s16le
du -sh "$O"/*
