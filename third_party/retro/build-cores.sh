#!/bin/bash
# Cross-build libretro cores for the NetCast TV (Cortex-A9, NEON, softfp, glibc 2.12).
cd "$(dirname "$0")"
. ./ra-env.sh
OUT=$PWD/cores; mkdir -p "$OUT"
ARCHF="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -static-libgcc -static-libstdc++"
build() {  # dir, makefile, output, extra args...
  d=$1; mf=$2; o=$3; shift 3
  ( cd "$d" && make -f "$mf" clean >/dev/null 2>&1; \
    make -j3 -f "$mf" platform=unix CC="arm-lgtv-linux-gnueabi-gcc $ARCHF" CXX="arm-lgtv-linux-gnueabi-g++ $ARCHF" \
         AR=arm-lgtv-linux-gnueabi-ar "$@" > "../log-$d.txt" 2>&1 \
    && cp "$o" "$OUT/" && echo "OK   $o" || echo "FAIL $d (see log-$d.txt)" )
}
build libretro-fceumm   Makefile.libretro fceumm_libretro.so &
build snes9x2005        Makefile          snes9x2005_libretro.so &
build gambatte-libretro Makefile          gambatte_libretro.so &
wait
build Genesis-Plus-GX   Makefile.libretro genesis_plus_gx_libretro.so &
build gpsp              Makefile          gpsp_libretro.so CPU_ARCH=arm HAVE_DYNAREC=1 &
build pcsx_rearmed      Makefile.libretro pcsx_rearmed_libretro.so ARCH=arm DYNAREC=ari64 HAVE_NEON=1 BUILTIN_GPU=neon &
wait
ls -la "$OUT"
