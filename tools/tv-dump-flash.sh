#!/bin/sh
# Read-only dump of the TV's flash to the USB stick. NEVER writes to flash/eMMC.
# Reads /dev/mtdblock* (and, only if explicitly named, eMMC partitions) and writes the
# images to $DST on the USB stick. Then a BusyBox httpd serves them for the host to pull.
#
#   tv-dump-flash.sh mtd           dump every MTD partition (NOR/NAND)
#   tv-dump-flash.sh emmc <names>  read-dump the listed eMMC partitions/devices by name
#   tv-dump-flash.sh serve [port]  (re)start the httpd on the dump dir
#
# This script only ever READS /dev/mtd*, /dev/mmcblk*, etc., and WRITES only under the
# USB stick. It never opens any flash node for writing.
set -u
# runs on the TV: the netcast/ folder of the stick (override with NETCAST_ROOT)
BASE=${NETCAST_ROOT:-$(ls -d /mnt/usb/usb*/Drive*/netcast 2>/dev/null | head -1)}
case "$BASE" in /mnt/usb/*/netcast) ;; *) echo "refusing: not on USB"; exit 1;; esac
DST="$BASE/flashdump"
BB="$BASE/busybox"
PORT=8099

serve() {
  P="${1:-$PORT}"
  pid=$($BB pidof httpd 2>/dev/null)
  [ -n "$pid" ] && $BB kill $pid 2>/dev/null
  $BB httpd -p "0.0.0.0:$P" -h "$DST"
  echo "serving $DST on http://TV:$P/"
  $BB ls -l "$DST"
}

dump_one() {  # src label
  src="$1"; label="$2"
  [ -r "$src" ] || { echo "skip $label: $src not readable"; return; }
  $BB dd if="$src" of="$DST/$label.bin" bs=65536 2>"/tmp/dd.$label"
  sz=$($BB stat -c %s "$DST/$label.bin" 2>/dev/null || echo 0)
  md5=$($BB md5sum "$DST/$label.bin" 2>/dev/null | $BB cut -d' ' -f1)
  echo "$label  $sz bytes  md5=$md5  <- $src"
}

$BB mkdir -p "$DST"

case "${1:-mtd}" in
  mtd)
    echo "=== /proc/mtd ==="; cat /proc/mtd
    n=0
    while [ $n -lt 64 ]; do
      if [ -e "/dev/mtd$n" ]; then
        name=$($BB sed -n "$((n+1))p" /proc/mtd 2>/dev/null | $BB tr -d '"' | $BB awk '{print $4}')
        [ -n "$name" ] || name="mtd$n"
        blk="/dev/mtdblock$n"; [ -e "$blk" ] || blk="/dev/mtd$n"
        dump_one "$blk" "mtd${n}_${name}"
      fi
      n=$((n+1))
    done
    serve
    ;;
  emmc)
    shift
    for p in "$@"; do
      path="$p"; [ -e "$path" ] || path="/dev/$p"
      dump_one "$path" "emmc_$($BB basename "$p")"
    done
    serve
    ;;
  serve)
    serve "${2:-$PORT}"
    ;;
  *)
    echo "usage: $0 mtd | emmc <names> | serve [port]"; exit 2
    ;;
esac
