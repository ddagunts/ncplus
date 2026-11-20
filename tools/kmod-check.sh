#!/bin/sh
# Compare our modules' struct module layout with one of LG's own modules, before any insmod.
#   tools/kmod-check.sh [lg-module-path-on-tv]      (default /mnt/lg/res/lglib/tfat.ko)
# Prints size of .gnu.linkonce.this_module and the init/exit relocation offsets for each.
set -e
TOP=$(cd "$(dirname "$0")/.." && pwd)
R=$TOP/third_party/kernel/armv7-eabihf--glibc--stable-2018.11-1/bin/arm-linux-readelf
LG=${1:-/mnt/lg/res/lglib/tfat.ko}
mkdir -p "$TOP/tv_dump/kmod"
L="$TOP/tv_dump/kmod/$(basename "$LG")"
[ -s "$L" ] || "$TOP/tools/pull.sh" "$LG" "$TOP/tv_dump/kmod"
layout() {
  sz=$($R -S -W "$1" | awk '/\.gnu\.linkonce\.this_module/ && !/rel/ {print $6}')
  rel=$($R -r -W "$1" | sed -n '/rel.gnu.linkonce.this_module/,/^$/p' | awk '/R_ARM/ {printf "%s@%s ", $5, $1}')
  echo "size=$sz $rel"
}
A=$(layout "$L"); echo "LG   $(basename "$L"): $A  $(strings "$L" | grep -m1 vermagic)"
rc=0
for k in "$TOP"/src/kmod/*.ko; do
  B=$(layout "$k"); echo "ours $(basename "$k"): $B"
  [ "$A" = "$B" ] || rc=1
done
[ $rc = 0 ] && echo "LAYOUT MATCHES" || echo "LAYOUT DIFFERS: do not insmod"
exit $rc
