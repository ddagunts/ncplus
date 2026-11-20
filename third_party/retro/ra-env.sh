# Cross-build environment for RetroArch and the libretro cores. Source it: . ./ra-env.sh
# EGL/GLES2 link against the stubs from src/Makefile (make -C src out/stubs/.ok).
_RA_TOP=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/../.." && pwd)
_RA_SDK=$_RA_TOP/toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot
export PATH=$_RA_SDK/bin:$PATH CROSS_COMPILE=arm-lgtv-linux-gnueabi- CC=arm-lgtv-linux-gnueabi-gcc CXX=arm-lgtv-linux-gnueabi-g++
export PKG_CONF_PATH=$_RA_SDK/bin/pkg-config PKG_CONFIG_LIBDIR=$_RA_TOP/third_party/sdl2-netcast/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=/
export CFLAGS="-O2 -I$_RA_TOP/third_party/khronos -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp" CXXFLAGS="-O2 -I$_RA_TOP/third_party/khronos -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp"
export LDFLAGS="-Wl,--as-needed -L$_RA_TOP/src/out/stubs -Wl,--allow-shlib-undefined -static-libgcc -static-libstdc++"
