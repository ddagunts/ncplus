#!/bin/sh
# Mount the NetCast chroot image (on this USB stick) at /tmp/ncroot and enter it.
# Writes land only in the image file on the USB stick and in RAM (tmpfs).
# No TV eMMC filesystem is mounted into the chroot; TV dirs are bound read-only.
D=$(cd "$(dirname "$0")" && pwd)
BB="$D/busybox"
IMG="$D/netcast-root.ext4"
R=/tmp/ncroot

case "$D" in /mnt/usb/*) ;; *) echo "refusing: $D is not on a USB stick" >&2; exit 1;; esac
[ -f "$IMG" ] || { echo "missing $IMG" >&2; exit 1; }

mounted() { $BB grep -q " $1 " /proc/mounts; }
die() { echo "enter.sh: $*" >&2; exit 1; }

if ! mounted $R; then
  $BB mkdir -p $R || die "mkdir $R"
  $BB mount -t ext4 -o loop,noatime "$IMG" $R || die "loop mount failed"
  $BB mount -t proc  proc  $R/proc
  $BB mount -t sysfs sysfs $R/sys
  $BB mount -o bind /dev     $R/dev
  $BB mount -o bind /dev/pts $R/dev/pts
  $BB mount -o bind /tmp     $R/tmp
  $BB mount -t tmpfs -o size=8m,mode=755 tmpfs $R/run
  # Read-only TV filesystems (squashfs/unionfs), same paths as on the TV.
  for p in /mnt/lg/res/lglib /mnt/vendor /mnt/addon /mnt/browser /usr/local /usr/local/var/run/dbus "$D"; do
    [ -d "$p" ] || continue
    $BB mkdir -p "$R$p" 2>/dev/null
    $BB mount -o bind "$p" "$R$p" || echo "warn: bind $p failed" >&2
  done
  $BB cat /etc/resolv.conf > $R/run/resolv.conf 2>/dev/null
fi

if [ $# -gt 0 ]; then
  exec $BB env -i HOME=/root TERM="${TERM:-vt100}" PATH=/usr/sbin:/usr/bin:/sbin:/bin $BB chroot $R /bin/sh -lc "$*"
fi
exec $BB env -i HOME=/root TERM="${TERM:-vt100}" PATH=/usr/sbin:/usr/bin:/sbin:/bin $BB chroot $R /bin/sh -l
