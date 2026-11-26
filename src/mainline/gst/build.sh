#!/bin/bash
# Build the netcastsink GStreamer plugin (netcastsink.c) for Debian trixie armhf:
#   src/mainline/gst/build.sh   -> third_party/mainline/out/libgstnetcastsink.so
# Headers and libraries come from trixie's armhf -dev packages (downloaded once into
# third_party/dl/gstdev, unpacked, not installed); the compiler is the kernel's toolchain
# (third_party/build-mainline.sh). third_party/build-debian.sh installs the result.
set -euo pipefail
TOP="$(cd "$(dirname "$0")/../../.." && pwd)"
DL="$TOP/third_party/dl/gstdev"
OUT="$TOP/third_party/mainline/out"
CC="$TOP/third_party/kernel/armv7-eabihf--glibc--stable-2018.11-1/bin/arm-linux-gcc"
M=https://deb.debian.org/debian/pool/main
DEBS=(
	"g/gstreamer1.0/libgstreamer1.0-dev_1.26.2-2_armhf.deb"
	"g/gstreamer1.0/libgstreamer1.0-0_1.26.2-2_armhf.deb"
	"g/gst-plugins-base1.0/libgstreamer-plugins-base1.0-dev_1.26.2-1+deb13u2_armhf.deb"
	"g/gst-plugins-base1.0/libgstreamer-plugins-base1.0-0_1.26.2-1+deb13u2_armhf.deb"
	"g/glib2.0/libglib2.0-0t64_2.84.4-3~deb13u5_armhf.deb"
	"g/glib2.0/libgio-2.0-dev_2.84.4-3~deb13u5_armhf.deb"
)
[ -x "$CC" ] || { echo "no $CC: run third_party/build-mainline.sh first" >&2; exit 1; }
mkdir -p "$DL/root" "$OUT"
for d in "${DEBS[@]}"; do
	f="$DL/${d##*/}"
	[ -f "$f" ] || curl -sSfL -o "$f" "$M/$d"
	dpkg-deb -x "$f" "$DL/root"
done
R="$DL/root/usr"
L="$R/lib/arm-linux-gnueabihf"
# The toolchain's glibc is older than trixie's: the libraries are only linked against
# (--allow-shlib-undefined), the plugin itself needs nothing newer.
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
	-I"$R/include/gstreamer-1.0" -I"$R/include/glib-2.0" -I"$L/glib-2.0/include" \
	"$TOP/src/mainline/gst/netcastsink.c" -o "$OUT/libgstnetcastsink.so" \
	-L"$L" -Wl,--allow-shlib-undefined -Wl,-rpath-link,"$L" \
	"$L/libgstvideo-1.0.so.0" "$L/libgstreamer-1.0.so.0" "$L/libgobject-2.0.so.0" "$L/libglib-2.0.so.0"
ls -la "$OUT/libgstnetcastsink.so"
