#!/bin/bash
# push.sh <remote-subdir> <localfile>...
# Upload files into <stick>/netcast/<remote-subdir>/ on the TV.
# Starts a busybox ftpd confined to the netcast/ dir on the USB stick, uploads, then kills it.
# The stick is found automatically (/mnt/usb/usb*/Drive*/netcast); override with NETCAST_ROOT.
# The TV's address: $NETCAST_TV or tools/.tv-host (see tv.py).
set -e
S=$(cd "$(dirname "$0")" && pwd)
T="python3 -I $S/tv.py"
TV=$(python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import tv; print(tv.tv_host())' "$S")
ROOT=${NETCAST_ROOT:-$($T 'ls -d /mnt/usb/usb*/Drive*/netcast 2>/dev/null | head -1')}
case "$ROOT" in /mnt/usb/*/netcast) ;; *) echo "push.sh: no netcast/ folder on a USB stick (got '$ROOT')" >&2; exit 1;; esac
sub="$1"; shift
case "$sub" in *..*|/*) echo "bad subdir"; exit 1;; esac
$T "mkdir -p '$ROOT/$sub' && cd / && (tcpsvd -E 0.0.0.0 2121 ftpd -w '$ROOT' </dev/null >/dev/null 2>&1 &); usleep 400000" >/dev/null
rc=0
for f in "$@"; do
  esub=$(printf '%s' "$sub" | sed 's/ /%20/g')
  curl -sS --ftp-pasv --ftp-create-dirs -u anonymous:x -T "$f" "ftp://$TV:2121/$esub/" || rc=1
done
# md5sum: the TV's own busybox has none; use the static one on the stick if present.
$T "killall tcpsvd 2>/dev/null; cd '$ROOT/$sub' && chmod +x * 2>/dev/null; MD5=md5sum; [ -x '$ROOT/busybox' ] && MD5='$ROOT/busybox md5sum'; \$MD5 *"
for f in "$@"; do md5sum "$f"; done
exit $rc
