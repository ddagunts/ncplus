#!/bin/sh
# webtest.sh start|stop|status: local test web server for the Web Browser (the TV has no internet access).
# The static BusyBox's httpd on 127.0.0.1 (port $WEBTEST_PORT, default 8090) serves netcast/test/ from the stick:
# browser test pages at /, and test media for the example links in netcast.conf at /video/, /audio/, /photos/.
# Loopback only, read-only, nothing written to the TV's flash.
N=$(cd "$(dirname "$0")/.." && pwd)
PORT=${WEBTEST_PORT:-8090}
PIDF=/tmp/webtest.pid
running() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }
case "$1" in
start)
  if running; then echo "webtest: already running: http://127.0.0.1:$PORT/"; exit 0; fi
  [ -x "$N/busybox" ] || { echo "webtest: needs the static busybox in $N (left out by --no-dev)" >&2; exit 1; }
  # BusyBox's built-in MIME table lacks the media types: without these, .mp4/.mkv are served as text/html
  printf '%s\n' .mp4:video/mp4 .m4v:video/mp4 .mkv:video/x-matroska .webm:video/webm .ts:video/mp2t \
    .mpg:video/mpeg .avi:video/x-msvideo .mov:video/quicktime .flac:audio/flac .mp3:audio/mpeg \
    .wav:audio/wav .ogg:audio/ogg .opus:audio/ogg .m3u:audio/x-mpegurl .css:text/css .js:text/javascript \
    .svg:image/svg+xml .webp:image/webp > /tmp/webtest.conf
  "$N/busybox" httpd -f -p "127.0.0.1:$PORT" -h "$N/test" -c /tmp/webtest.conf < /dev/null > /dev/null 2>&1 &
  echo $! > "$PIDF"
  usleep 300000
  if running; then echo "webtest: http://127.0.0.1:$PORT/"; else echo "webtest: httpd failed to start" >&2; rm -f "$PIDF"; exit 1; fi
  ;;
stop)
  running && kill "$(cat "$PIDF")"
  rm -f "$PIDF"
  ;;
status)
  if running; then echo "webtest: running (pid $(cat "$PIDF")): http://127.0.0.1:$PORT/"; else echo "webtest: stopped"; exit 1; fi
  ;;
*)
  echo "usage: webtest.sh start|stop|status" >&2; exit 2
  ;;
esac
