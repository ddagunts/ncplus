#!/bin/bash
# Build static SDL2 2.32.10 with the LG NetCast ("netcast") video driver.
# Output: third_party/sdl2-netcast/{include,lib}. Needs the buildroot-nc4 SDK in toolchain/.
set -euo pipefail
cd "$(dirname "$0")"
TP=$PWD
TOP=$(cd .. && pwd)
V=2.32.10
CMAKE=$TP/cmake-3.31.10-linux-x86_64/bin/cmake
if [ ! -x "$CMAKE" ]; then
  curl -sSfLO https://github.com/Kitware/CMake/releases/download/v3.31.10/cmake-3.31.10-linux-x86_64.tar.gz
  tar xzf cmake-3.31.10-linux-x86_64.tar.gz
fi
[ -f SDL2-$V.tar.gz ] || curl -sSfLO https://github.com/libsdl-org/SDL/releases/download/release-$V/SDL2-$V.tar.gz
echo "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165  SDL2-$V.tar.gz" | sha256sum -c -
if [ ! -f SDL2-$V/.netcast-patched ]; then
  rm -rf SDL2-$V && tar xzf SDL2-$V.tar.gz
  patch -d SDL2-$V -p1 < patches/sdl2-$V-netcast.patch
  touch SDL2-$V/.netcast-patched
fi
rm -rf sdl2-build && mkdir sdl2-build && cd sdl2-build
$CMAKE ../SDL2-$V -DCMAKE_TOOLCHAIN_FILE=$TOP/toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$TP/sdl2-netcast \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF \
  -DSDL_NETCAST=ON -DSDL_NETCAST_LIBNC_DIR=$TOP/src/libnc \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_RPI=OFF -DSDL_VIVANTE=OFF -DSDL_DIRECTFB=OFF -DSDL_OFFSCREEN=OFF \
  -DSDL_OPENGL=OFF -DSDL_OPENGLES=ON -DSDL_VULKAN=OFF \
  -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF -DSDL_OSS=ON \
  -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_HIDAPI=OFF -DSDL_LIBSAMPLERATE=OFF >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "installed: $TP/sdl2-netcast/lib/libSDL2.a"
