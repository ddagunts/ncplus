#!/bin/bash
# Build pvrsrvkm.ko, the PowerVR SGX544 services kernel module (DDK 1.9), for the mainline
# kernel on the MT5398 TV.
#
#   third_party/build-sgx.sh [KBUILD_DIR]     # default: third_party/mainline/linux-6.12.119/kbuild
#
# Source: TI's open release of the 1.9 kernel module (tag TI_LINUX_OMAP_SGX_DDK_1.9_2253347,
# Dual MIT/GPLv2), the closest open DDK to LG's 1.9@2291151. It is cloned into
# third_party/sgx-km (a git work tree, branch "netcast"), then:
#   src/mainline/sgx/patches/*.patch     changes to TI's files (git am), exported from that branch
#   src/mainline/sgx/{Kbuild,config_kernel.h}, src/mainline/sgx/system/mt5398/
#                                        our files, copied in (not part of the branch)
# Also, for LG's soft-float (armel) userspace, with the openlgtv toolchain in toolchain/:
#   src/mainline/sgx/test/sgxgltest.c     off-screen GLES2 first-frame test
#   src/mainline/sgx/test/sgxscan.c       rendering into KMS buffers (no display needed)
#   src/mainline/sgx/test/sgxflip.c       page flips on the panel (stop the desktop first)
#   src/mainline/sgx/test/sgxupload.c     how fast CPU pixels reach the GPU
#   src/mainline/sgx/wsegl/netcast_wsegl.c our EGL window-system module
# Output: third_party/mainline/out/sgx/{pvrsrvkm.ko, sgxgltest, libpvrEWS_NullWSEGL.so,
#         pvrtest.sh}: everything for <stick>/netcast/mainline/pvr/ except the TV's own
#         libraries (fire.sh copies those) and debs/.
# The kernel must have CONFIG_MODULES=y.
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
TAG=TI_LINUX_OMAP_SGX_DDK_1.9_2253347
URL=https://git.ti.com/git/graphics/omap5-sgx-ddk-linux.git
W="$TOP/third_party/sgx-km"
SG="$TOP/src/mainline/sgx"
SRC="$TOP/third_party/mainline/linux-6.12.119"
KB="${1:-$SRC/kbuild}"
OUT="$TOP/third_party/mainline/out/sgx"
TC="$TOP/third_party/kernel/armv7-eabihf--glibc--stable-2018.11-1/bin/arm-linux-"

grep -qx 'CONFIG_MODULES=y' "$KB/.config" || { echo "$KB: kernel needs CONFIG_MODULES=y" >&2; exit 1; }

if [ ! -d "$W/.git" ]; then
	git clone -q --branch "$TAG" "$URL" "$W"
	git -C "$W" checkout -q -b netcast
	shopt -s nullglob
	p=("$SG"/patches/*.patch)
	[ ${#p[@]} = 0 ] || git -C "$W" am -q "${p[@]}"
fi

cp "$SG/Kbuild" "$SG/config_kernel.h" "$W/"
mkdir -p "$W/services4/system/mt5398"
cp "$SG"/system/mt5398/* "$W/services4/system/mt5398/"

# Built in a copy under a fixed path: the module keeps its source paths (__FILE__), and GCC 7
# can't remap them.
B=/var/tmp/netcast-sgx-build
rm -rf "$B"
mkdir -m 700 "$B"
trap 'rm -rf "$B"' EXIT
(cd "$W" && find . -path ./.git -prune -o -type f \( -name '*.[ch]' -o -name Kbuild -o -name '*.mk' \) -print0 \
	| tar cf - --null -T -) | tar xf - -C "$B"
make -C "$SRC" O="$KB" ARCH=arm CROSS_COMPILE="$TC" M="$B" KBUILD_MODPOST_WARN=1 \
	-j"$(nproc)" modules
mkdir -p "$OUT"
"${TC}strip" --strip-debug -o "$OUT/pvrsrvkm.ko" "$B/pvrsrvkm.ko"

UCC="$TOP/toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot/bin/arm-lgtv-linux-gnueabi-gcc"
if [ -x "$UCC" ]; then
	"$UCC" -O2 -Wall -o "$OUT/sgxgltest" "$SG/test/sgxgltest.c" -ldl -lrt
	"$UCC" -std=gnu99 -O2 -Wall -o "$OUT/sgxscan" "$SG/test/sgxscan.c" -ldl -lrt
	"$UCC" -std=gnu99 -O2 -Wall -o "$OUT/sgxflip" "$SG/test/sgxflip.c" -ldl -lrt -lm -lpthread
	"$UCC" -std=gnu99 -O2 -Wall -o "$OUT/sgxupload" "$SG/test/sgxupload.c" -ldl -lrt
	"$UCC" -O2 -Wall -fPIC -shared -o "$OUT/libpvrEWS_NullWSEGL.so" "$SG/wsegl/netcast_wsegl.c" -ldl -lrt
else
	echo "no $UCC: sgxgltest and the WSEGL not built" >&2
fi
cp "$SG/pvrtest.sh" "$OUT/"

# Debian trixie's armel C runtime, for debian.img builds without armel multiarch
# (pvrtest.sh unpacks it). Pinned: hashes from the signed trixie index (13.8).
DEBS="$TOP/third_party/dl/armel"
mkdir -p "$DEBS" "$OUT/debs"
while read -r sum path; do
	f="$DEBS/${path##*/}"
	[ -f "$f" ] || curl -sSfL -o "$f" "http://deb.debian.org/debian/$path"
	echo "$sum  $f" | sha256sum -c --quiet - || { rm -f "$f"; exit 1; }
	cp "$f" "$OUT/debs/"
done <<'PINS'
e6d49111722906c475ba891b613161f1359e4d15e5e86efc68533bbf846eafdb pool/main/g/glibc/libc6_2.41-12+deb13u5_armel.deb
2a7efceac4acc13ee23ff73fee1ca3e865baa2b09b9537f0d8496fac7dff7d4f pool/main/g/gcc-14/libgcc-s1_14.2.0-19_armel.deb
PINS
ls -la "$OUT" "$OUT/debs"
