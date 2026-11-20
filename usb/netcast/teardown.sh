#!/bin/sh
# Unmount everything enter.sh mounted (innermost first). Exit any chroot shells first.
D=$(cd "$(dirname "$0")" && pwd)
BB="$D/busybox"
R=/tmp/ncroot
$BB sync
# Unmount every mount under $R, deepest path first.
for m in $($BB awk -v r="$R" '$2==r || index($2, r"/")==1 {print $2}' /proc/mounts | $BB sort -r); do
  $BB umount -d "$m" || echo "warn: could not umount $m (busy? try: fuser -m $m)" >&2
done
$BB grep -q " $R " /proc/mounts && { echo "still mounted: $R" >&2; exit 1; }
$BB rmdir $R 2>/dev/null
echo "ncroot unmounted"
