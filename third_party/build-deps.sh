#!/bin/bash
# Fetch (pinned + checksummed) and cross-build every third-party component NetCast+ ships.
#
#   third_party/build-deps.sh            all steps, in order
#   third_party/build-deps.sh doom rdp   only the named steps
#
# Steps: sdl2 mixer ttf smb2 vterm lvgl doom tyrian rdp retroarch ffmpeg   (optional, not in the default list: kmod)
# Needs: the openlgtv buildroot-nc4 SDK in toolchain/ (see README), curl, git, unzip, python3, a host gcc.
# Binaries are staged in src/out/<app>/ where build/package.sh picks them up.
# Sources already unpacked are reused, so re-running a step only rebuilds it.
set -euo pipefail
cd "$(dirname "$0")"
TP=$PWD
TOP=$(cd .. && pwd)
SDK=$TOP/toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot
SYSROOT=$SDK/arm-lgtv-linux-gnueabi/sysroot
X=$SDK/bin/arm-lgtv-linux-gnueabi-
TC=$SDK/share/buildroot/toolchainfile.cmake
CM=$TP/cmake-3.31.10-linux-x86_64/bin/cmake
P=$TP/sdl2-netcast
OUT=$TOP/src/out
J=$(nproc)
DL=$TP/dl
mkdir -p "$DL" "$OUT"

[ -x "${X}gcc" ] || { echo "missing SDK: $SDK (see README, Building)" >&2; exit 1; }

log() { printf '\n== %s\n' "$*"; }

# fetch <url> <file> <sha256>
fetch() {
  [ -f "$DL/$2" ] || curl -sSfL -o "$DL/$2.part" "$1" && { [ -f "$DL/$2" ] || mv "$DL/$2.part" "$DL/$2"; }
  echo "$3  $DL/$2" | sha256sum -c --quiet - || { echo "checksum mismatch: $2" >&2; exit 1; }
}
# unpack <file> <dir>: extract once
unpack() { [ -d "$TP/$2" ] || tar -C "$TP" -xf "$DL/$1"; }
# gitpin <url> <dir> <commit>
gitpin() {
  if [ ! -d "$2/.git" ]; then git init -q "$2" && git -C "$2" remote add origin "$1"; fi
  if [ "$(git -C "$2" rev-parse HEAD 2>/dev/null)" != "$3" ]; then
    git -C "$2" fetch -q --depth 1 origin "$3" && git -C "$2" checkout -q FETCH_HEAD
  fi
}
cmake_build() {  # <builddir> <srcdir> args...
  local b=$1 s=$2; shift 2
  rm -rf "$TP/$b" && mkdir "$TP/$b"
  (cd "$TP/$b" && "$CM" "$TP/$s" -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release "$@" > cmake.log 2>&1 \
     || { tail -30 cmake.log; exit 1; }
   make -j"$J" > build.log 2>&1 || { grep -E "error" build.log | head -20; exit 1; })
}

step_sdl2() {  # SDL2 2.32.10 + netcast video/audio drivers (also fetches CMake)
  log "SDL2"; "$TP/build-sdl2.sh"
}

step_mixer() {
  log "SDL2_mixer 2.8.2"
  fetch https://github.com/libsdl-org/SDL_mixer/releases/download/release-2.8.2/SDL2_mixer-2.8.2.tar.gz \
        SDL2_mixer-2.8.2.tar.gz 938dff531d00ace2296557a6599abe6f34599e2f34f0a4a08a397e2ccac8b8f7
  unpack SDL2_mixer-2.8.2.tar.gz SDL2_mixer-2.8.2
  cmake_build mixer-build SDL2_mixer-2.8.2 -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_PREFIX_PATH="$P" \
    -DCMAKE_FIND_ROOT_PATH="$P" -DSDL2_DIR="$P/lib/cmake/SDL2" -DBUILD_SHARED_LIBS=OFF \
    -DSDL2MIXER_SAMPLES=OFF -DSDL2MIXER_VENDORED=OFF -DSDL2MIXER_DEPS_SHARED=OFF -DSDL2MIXER_CMD=OFF \
    -DSDL2MIXER_FLAC=OFF -DSDL2MIXER_GME=OFF -DSDL2MIXER_MOD=OFF -DSDL2MIXER_MP3=OFF -DSDL2MIXER_MIDI=OFF \
    -DSDL2MIXER_OPUS=OFF -DSDL2MIXER_VORBIS=OFF -DSDL2MIXER_WAVPACK=OFF -DSDL2MIXER_WAVE=ON
  make -C "$TP/mixer-build" install > /dev/null
}

step_ttf() {
  log "SDL2_ttf 2.24.0"
  fetch https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.tar.gz \
        SDL2_ttf-2.24.0.tar.gz 0b2bf1e7b6568adbdbc9bb924643f79d9dedafe061fa1ed687d1d9ac4e453bfd
  unpack SDL2_ttf-2.24.0.tar.gz SDL2_ttf-2.24.0
  cmake_build ttf-build SDL2_ttf-2.24.0 -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_PREFIX_PATH="$P" \
    -DCMAKE_FIND_ROOT_PATH="$P" -DSDL2_DIR="$P/lib/cmake/SDL2" -DBUILD_SHARED_LIBS=OFF \
    -DSDL2TTF_SAMPLES=OFF -DSDL2TTF_VENDORED=OFF -DSDL2TTF_HARFBUZZ=OFF -DSDL2TTF_FREETYPE=ON
  make -C "$TP/ttf-build" install > /dev/null
}

step_smb2() {
  log "libsmb2 6.3"
  fetch https://github.com/sahlberg/libsmb2/archive/refs/tags/libsmb2-6.3.tar.gz \
        libsmb2-6.3.tar.gz 5011003fd091513f897e1ebbb8e1a40af634386df7199716348b7f77569a2790
  unpack libsmb2-6.3.tar.gz libsmb2-libsmb2-6.3
  cmake_build smb2-build libsmb2-libsmb2-6.3 -DBUILD_SHARED_LIBS=OFF -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF \
    -DENABLE_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$TP/smb2-netcast"
  make -C "$TP/smb2-build" install > /dev/null
}

step_vterm() {  # sources only; src/Makefile compiles them
  log "libvterm 0.3.3"
  fetch https://launchpad.net/libvterm/trunk/v0.3/+download/libvterm-0.3.3.tar.gz \
        libvterm-0.3.3.tar.gz 09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0
  unpack libvterm-0.3.3.tar.gz libvterm-0.3.3
}

step_lvgl() {  # sources only; src/Makefile compiles them with src/lvgl/lv_conf.h
  log "LVGL 9.6.0"
  gitpin https://github.com/lvgl/lvgl.git "$TP/lvgl" 80ca777e37a2b176770726a02e07a6fb79ef0b39
}

step_doom() {
  log "Chocolate Doom 3.1.1 + Freedoom 0.13.0"
  fetch https://github.com/chocolate-doom/chocolate-doom/archive/refs/tags/chocolate-doom-3.1.1.tar.gz \
        chocolate-doom-3.1.1.tar.gz 1edcc41254bdc194beb0d33e267fae306556c4d24110a1d3d3f865717f25da23
  fetch https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip \
        freedoom-0.13.0.zip 3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59
  unpack chocolate-doom-3.1.1.tar.gz chocolate-doom-chocolate-doom-3.1.1
  cmake_build doom-build chocolate-doom-chocolate-doom-3.1.1 -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=OFF \
    -DENABLE_SDL2_NET=OFF -DENABLE_SDL2_MIXER=ON \
    -DSDL2_INCLUDE_DIR="$P/include/SDL2" -DSDL2_LIBRARY="$P/lib/libSDL2.a" -DSDL2_MAIN_LIBRARY="$P/lib/libSDL2main.a" \
    -DSDL2_MIXER_INCLUDE_DIR="$P/include/SDL2" -DSDL2_MIXER_LIBRARY="$P/lib/libSDL2_mixer.a" \
    -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON -DCMAKE_DISABLE_FIND_PACKAGE_SampleRate=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_FluidSynth=ON \
    -DCMAKE_C_STANDARD_LIBRARIES="-ldl -lpthread -lm -lrt" -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc"
  mkdir -p "$OUT/doom"
  "${X}strip" -o "$OUT/doom/chocolate-doom" "$TP/doom-build/src/chocolate-doom"
  unzip -q -o -j "$DL/freedoom-0.13.0.zip" freedoom-0.13.0/freedoom1.wad -d "$OUT/doom"
  unzip -q -o -p "$DL/freedoom-0.13.0.zip" freedoom-0.13.0/COPYING.txt > "$OUT/doom/FREEDOOM-COPYING.txt"
}

step_tyrian() {  # engine only; the freeware game data is fetched by package.sh --with-tyrian-data
  log "OpenTyrian 2.1.20260913"
  fetch https://github.com/opentyrian/opentyrian/archive/refs/tags/v2.1.20260913.tar.gz \
        opentyrian-2.1.20260913.tar.gz dbcd96383d4fa571137242c36bd7eca054cf5a08a9bf2eec15ef230d6e60680d
  unpack opentyrian-2.1.20260913.tar.gz opentyrian-2.1.20260913
  (cd "$TP/opentyrian-2.1.20260913" && rm -f opentyrian && make -j"$J" CC="${X}gcc" PLATFORM=UNIX \
     WITH_NETWORK=false TYRIAN_DIR=data VCS_IDREV="echo v2.1.20260913" \
     SDL_CPPFLAGS="-I$P/include/SDL2 -D_REENTRANT" SDL_LDFLAGS="-static-libgcc" \
     SDL_LDLIBS="$P/lib/libSDL2.a -ldl -lpthread -lrt" > build.log 2>&1 || { grep error build.log; exit 1; })
  mkdir -p "$OUT/tyrian"
  "${X}strip" -o "$OUT/tyrian/opentyrian" "$TP/opentyrian-2.1.20260913/opentyrian"
}

step_rdp() {
  log "FreeRDP 3.33.1 (SDL2 client)"
  fetch https://github.com/FreeRDP/FreeRDP/archive/refs/tags/3.33.1.tar.gz \
        freerdp-3.33.1.tar.gz 6b6da1e7217f924b033da6896e70eb4c4a447c3900d03c0d10179062f0a7d495
  unpack freerdp-3.33.1.tar.gz FreeRDP-3.33.1
  local S=$SYSROOT L=$TP/freerdp-staticlibs
  # The TV has no libatomic.so: a copy of the static archive under the .so name makes -latomic link it statically.
  mkdir -p "$L" && cp "$SDK/arm-lgtv-linux-gnueabi/sysroot/lib/libatomic.a" "$L/libatomic.so"
  # glibc 2.12 has no <threads.h>; the SDK has no gcc-ar for C++, so no LTO.
  local F="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D__STDC_NO_THREADS__"
  PKG_CONFIG_SYSROOT_DIR=/ PKG_CONFIG_LIBDIR=$P/lib/pkgconfig cmake_build freerdp-build FreeRDP-3.33.1 \
    -DCMAKE_INSTALL_PREFIX="$TP/freerdp-netcast" -DCMAKE_PREFIX_PATH="$P" -DCMAKE_FIND_ROOT_PATH="$P;$S" \
    -DSDL2_DIR="$P/lib/cmake/SDL2" \
    -DOPENSSL_USE_STATIC_LIBS=TRUE -DOPENSSL_ROOT_DIR="$S/usr" -DZLIB_LIBRARY="$S/usr/lib/libz.a" \
    -DFREETYPE_LIBRARY="$S/usr/lib/libfreetype.a" -DFREETYPE_LIBRARY_RELEASE="$S/usr/lib/libfreetype.a" \
    -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DCMAKE_C_STANDARD_LIBRARIES="$S/usr/lib/libz.a -ldl -lrt -lpthread" \
    -DCMAKE_CXX_STANDARD_LIBRARIES="$S/usr/lib/libbz2.a $S/usr/lib/libz.a -l:libatomic.a -ldl -lrt -lpthread" \
    -DFREETYPE_INCLUDE_DIRS="$S/usr/include/freetype2" \
    -DBUILD_SHARED_LIBS=OFF -DWITH_CLIENT_SDL=ON -DWITH_CLIENT_SDL2=ON -DWITH_CLIENT_SDL3=OFF \
    -DWITH_SDL_LINK_SHARED=OFF -DWITH_SDL_IMAGE_DIALOGS=OFF -DWITH_X11=OFF -DWITH_WAYLAND=OFF -DWITH_SERVER=OFF \
    -DWITH_SAMPLE=OFF -DWITH_MANPAGES=OFF -DWITH_FFMPEG=OFF -DWITH_SWSCALE=OFF -DWITH_DSP_FFMPEG=OFF \
    -DWITH_VIDEO_FFMPEG=OFF -DWITH_CAIRO=OFF -DWITH_PULSE=OFF -DWITH_ALSA=OFF -DWITH_OSS=OFF -DWITH_SNDIO=OFF \
    -DWITH_CUPS=OFF -DWITH_PCSC=OFF -DWITH_KRB5=OFF -DWITH_FUSE=OFF -DWITH_PKCS11=OFF -DWITH_JSON_DISABLED=ON \
    -DWITH_AAD=OFF -DWITH_WEBVIEW=OFF -DWITH_SSO_MIB=OFF -DWITH_URIPARSER=OFF -DWITH_OPENH264=OFF -DWITH_JPEG=OFF \
    -DWITH_SIMD=ON -DWITH_UNICODE_BUILTIN=ON -DWITH_ICU=OFF -DCHANNEL_URBDRC=OFF -DWITH_LIBSYSTEMD=OFF \
    -DUSE_UNWIND=OFF -DWITH_PROXY=OFF -DWITH_SHADOW=OFF -DWITH_PLATFORM_SERVER=OFF -DWITH_RDTK=OFF \
    -DWITH_TIMEZONE_ICU=OFF -DCMAKE_EXE_LINKER_FLAGS="-L$L -static-libgcc -static-libstdc++"
  mkdir -p "$OUT/rdp"
  "${X}strip" -o "$OUT/rdp/sdl-freerdp" "$TP/freerdp-build/client/SDL/SDL2/sdl-freerdp"
}

step_ffmpeg() {  # FFmpeg + ffplay: software decode on the CPU (NEON), YUV->RGB on the GPU via SDL's GLES2 renderer
  log "FFmpeg 9.1 (ffplay, ffprobe, ffmpeg)"
  fetch https://ffmpeg.org/releases/ffmpeg-9.1.tar.xz ffmpeg-9.1.tar.xz \
        1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433
  unpack ffmpeg-9.1.tar.xz ffmpeg-9.1
  # ffplay: remote OK pauses; exit unregisters from LG's procmgr and skips LG's (hanging) destructors
  if [ ! -f "$TP/ffmpeg-9.1/.netcast-patched" ]; then
    patch -d "$TP/ffmpeg-9.1" -p1 < "$TP/patches/ffmpeg-9.1-ffplay-netcast.patch" && touch "$TP/ffmpeg-9.1/.netcast-patched"
  fi
  # The TV has no OpenSSL 3 / new zlib: a -L dir holding only the static archives makes -lssl etc. link statically.
  local S=$SYSROOT L=$TP/ffmpeg-staticlibs
  mkdir -p "$L" && for a in ssl crypto z bz2; do ln -sf "$S/usr/lib/lib$a.a" "$L/lib$a.a"; done
  rm -rf "$TP/ffmpeg-build" && mkdir "$TP/ffmpeg-build"
  (cd "$TP/ffmpeg-build" && PKG_CONFIG_LIBDIR=$P/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=/ \
     "$TP/ffmpeg-9.1/configure" --prefix="$TP/ffmpeg-netcast" --enable-cross-compile --cross-prefix="$X" \
     --arch=arm --cpu=cortex-a9 --target-os=linux --sysroot="$S" --pkg-config="$SDK/bin/pkg-config" --pkg-config-flags=--static \
     --enable-static --disable-shared --disable-doc --enable-version3 \
     --enable-ffplay --enable-ffprobe --enable-ffmpeg --enable-sdl2 --enable-openssl --enable-zlib --enable-bzlib \
     --enable-neon --enable-vfp --enable-runtime-cpudetect \
     --disable-xlib --disable-libxcb --disable-vaapi --disable-vdpau --disable-vulkan --disable-v4l2-m2m \
     --disable-alsa --disable-sndio --disable-indev=v4l2,fbdev,oss --disable-outdevs --disable-iconv \
     --disable-encoders --enable-encoder=wrapped_avframe,rawvideo,pcm_s16le --disable-muxers --enable-muxer=null,wav \
     --extra-cflags=-DNETCAST --extra-ldflags="-L$L -static-libgcc" --extra-libs="-ldl -lpthread -lm -lrt" > configure.log 2>&1 \
     || { tail -30 configure.log; tail -40 ffbuild/config.log; exit 1; }
   grep -qx 'CONFIG_FFPLAY=yes' ffbuild/config.mak || { echo "ffplay disabled (SDL2 not found?)" >&2; exit 1; }
   make -j"$J" > build.log 2>&1 || { grep -E "error|undefined" build.log | head -20; exit 1; })
  mkdir -p "$OUT/ffmpeg"
  for b in ffplay ffprobe ffmpeg; do "${X}strip" -o "$OUT/ffmpeg/$b" "$TP/ffmpeg-build/$b"; done
}

step_netsurf() {  # NetSurf 3.11 framebuffer browser on SDL2 (libnsfb SDL2 surface, remote-driven pointer)
  log "NetSurf 3.11 (+ libpng 1.6.59, libjpeg-turbo 3.2.1)"
  fetch https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz \
        netsurf-all-3.11.tar.gz 4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b
  fetch https://download.sourceforge.net/libpng/libpng-1.6.59.tar.xz libpng-1.6.59.tar.xz \
        d80dd2a38a37f803cb9b6ac7b14bd6e74ddc3b654780a8380bdf93523fdb4389
  fetch https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.1/libjpeg-turbo-3.2.1.tar.gz \
        libjpeg-turbo-3.2.1.tar.gz c27ad72a68da151177929aa969ae773a2da5ac2af9ce946079314fdac8cece7f
  unpack netsurf-all-3.11.tar.gz netsurf-all-3.11
  unpack libpng-1.6.59.tar.xz libpng-1.6.59
  unpack libjpeg-turbo-3.2.1.tar.gz libjpeg-turbo-3.2.1
  local NS=$TP/netsurf-all-3.11 D=$TP/netsurf-deps H=$TP/netsurf-hostdeps S=$SYSROOT PC=$TP/netsurf-deps/pc
  if [ ! -f "$NS/.netcast-patched" ]; then
    patch -d "$NS" -p1 < "$TP/patches/netsurf-3.11-netcast.patch" && touch "$NS/.netcast-patched"
  fi
  # target image libraries (static)
  cmake_build png-build libpng-1.6.59 -DCMAKE_INSTALL_PREFIX="$D" -DCMAKE_INSTALL_LIBDIR=lib \
    -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF \
    -DZLIB_LIBRARY="$S/usr/lib/libz.a" -DZLIB_INCLUDE_DIR="$S/usr/include"
  make -C "$TP/png-build" install > /dev/null
  cmake_build jpeg-build libjpeg-turbo-3.2.1 -DCMAKE_INSTALL_PREFIX="$D" -DCMAKE_INSTALL_LIBDIR=lib \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
  make -C "$TP/jpeg-build" install > /dev/null
  # NetSurf adds plain -lz/-lexpat; the TV has zlib 1.2.3 (no ZLIB_1.2.3.4 symbols) and no expat, so put the
  # static archives in a -L dir with no .so: the linker then picks them over the sysroot's shared ones.
  ln -sf "$S/usr/lib/libz.a" "$S/usr/lib/libexpat.a" "$D/lib/"
  # host libpng for NetSurf's build-time convert_image tool (the host has no libpng-dev)
  rm -rf "$TP/png-host-build" && mkdir "$TP/png-host-build"
  (cd "$TP/png-host-build" && "$CM" "$TP/libpng-1.6.59" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$H" \
     -DCMAKE_INSTALL_LIBDIR=lib -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF > cmake.log 2>&1 \
   && make -j"$J" install > build.log 2>&1) || { tail -20 "$TP/png-host-build/build.log"; exit 1; }
  # pkg-config files with full static-archive paths: the TV has none of these libraries (or older ones)
  mkdir -p "$PC"
  pcf() { printf 'Name: %s\nDescription: %s\nVersion: %s\nCflags: %s\nLibs: %s\n' "$1" "$1" "$2" "$3" "$4" > "$PC/$1.pc"; }
  # NetSurf adds -lm/-lz early in LDFLAGS but the static archives come late, so wrap each set of static
  # libraries in a linker group: circular refs (libpng<->libm/libz, curl<->ssl<->crypto<->z) resolve either way.
  grp() { echo "-Wl,--start-group $* -Wl,--end-group"; }
  local SSL="$S/usr/lib/libssl.a $S/usr/lib/libcrypto.a -ldl -lpthread"
  pcf libcurl 8.0.0 "-I$S/usr/include" "$(grp "$S/usr/lib/libcurl.a $SSL $S/usr/lib/libz.a $S/lib/libatomic.a -lm")"
  pcf openssl 3.0.0 "-I$S/usr/include" "$(grp "$SSL")"
  pcf expat 2.5.0 "-I$S/usr/include" "$S/usr/lib/libexpat.a"
  pcf zlib 1.3.0 "-I$S/usr/include" "$S/usr/lib/libz.a"
  pcf freetype2 26.0.0 "-I$S/usr/include/freetype2" "$(grp "$S/usr/lib/libfreetype.a $S/usr/lib/libbz2.a $S/usr/lib/libz.a -lm")"
  pcf libpng 1.6.59 "-I$D/include" "$(grp "$D/lib/libpng16.a $S/usr/lib/libz.a -lm")"
  pcf sdl2 2.32.10 "-I$P/include/SDL2 -D_REENTRANT" "$P/lib/libSDL2.a -ldl -lpthread -lm -lrt"
  # the cross pkg-config sees only the files above plus NetSurf's own freshly installed libraries
  local NSPC="env PKG_CONFIG_LIBDIR=$NS/inst-framebuffer/lib/pkgconfig:$PC PKG_CONFIG_SYSROOT_DIR= pkg-config"
  cat > "$NS/netsurf/Makefile.config" <<EOF
override NETSURF_FB_FRONTEND := sdl
override NETSURF_FB_FONTLIB := freetype
override NETSURF_USE_JPEGXL := NO
override NETSURF_USE_WEBP := NO
override NETSURF_USE_VIDEO := NO
override NETSURF_USE_ROSPRITE := NO
override NETSURF_FB_RESPATH := \$\${HOME}/.netsurf/:\$\${NETSURFRES}
override NETSURF_FB_FONTPATH := \$\${NETSURFRES}/fonts
CFLAGS += -I$D/include -O2 -DNETCAST
LDFLAGS += -L$D/lib -static-libgcc
EOF
  # host pkg-config (pkgconf) is wrapped so both the libraries and netsurf find our .pc files;
  # the cross compiler comes from the SDK, convert_image/nsgenbind are built with the host cc.
  mkdir -p "$TP/netsurf-deps/bin"
  printf '#!/bin/sh\nexec %s "$@"\n' "$NSPC" > "$TP/netsurf-deps/bin/arm-lgtv-linux-gnueabi-pkg-config"
  chmod +x "$TP/netsurf-deps/bin/arm-lgtv-linux-gnueabi-pkg-config"
  ln -sf "$SDK/bin/pkgconf" "$TP/netsurf-deps/bin/pkg-config"
  # The SDK's arm-lgtv-*-gcc is a symlink to toolchain-wrapper (which dispatches on argv[0]); NetSurf's
  # buildsystem realpath()s the compiler and runs it by that resolved name, so the wrapper no longer knows
  # it's gcc and toolchain detection fails. Real-file shims (realpath resolves to themselves) call the SDK
  # tools by their proper names.
  for t in gcc g++ cpp ar ranlib strip nm as ld objcopy objdump; do
    printf '#!/bin/sh\nexec "%sbin/arm-lgtv-linux-gnueabi-%s" "$@"\n' "$SDK/" "$t" > "$TP/netsurf-deps/bin/arm-lgtv-linux-gnueabi-$t"
    chmod +x "$TP/netsurf-deps/bin/arm-lgtv-linux-gnueabi-$t"
  done
  # NetSurf's buildsystem calls plain `pkg-config` with PKG_CONFIG_PATH: LIBDIR must exclude host/sysroot .pc files.
  # The libraries get the cross compiler from HOST, but the browser's own Makefile only takes CC (its framebuffer
  # frontend otherwise falls back to the host `cc`), so CC/CXX go to that sub-make alone via NETSURF_CONFIG.
  rm -f "$NS/inst-framebuffer/build-stamp" "$NS/netsurf/nsfb"   # else netsurf-all skips the (re)link
  (cd "$NS" && export PATH="$TP/netsurf-deps/bin:$SDK/bin:$PATH" \
     PKG_CONFIG_LIBDIR="$NS/inst-framebuffer/lib/pkgconfig:$PC" PKG_CONFIG_SYSROOT_DIR= &&
   make -j"$J" TARGET=framebuffer HOST=arm-lgtv-linux-gnueabi BUILD=x86_64-linux-gnu \
     HOST_CC=cc BUILD_CC=cc \
     NETSURF_CONFIG="CC=$TP/netsurf-deps/bin/arm-lgtv-linux-gnueabi-gcc CXX=$TP/netsurf-deps/bin/arm-lgtv-linux-gnueabi-g++" \
     BUILD_LIBPNG_CFLAGS="-I$H/include" BUILD_LIBPNG_LDFLAGS="-Wl,--start-group -L$H/lib -lpng -lz -lm -Wl,--end-group" \
     > "$TP/netsurf-build.log" 2>&1) \
     || { grep -E "error|Error|undefined|failed" "$TP/netsurf-build.log" | head -30; exit 1; }
  mkdir -p "$OUT/netsurf"
  "${X}strip" -o "$OUT/netsurf/netsurf" "$NS/netsurf/nsfb"
}

step_kmod() {  # optional: vanilla 3.0.13 prepared with the TV's vermagic, then src/kmod (not loaded automatically)
  log "kernel modules (linux 3.0.13)"
  local K=$TP/kernel BL=armv7-eabihf--glibc--stable-2018.11-1
  mkdir -p "$K"
  [ -f "$K/linux-3.0.13.tar.xz" ] || curl -sSfL -o "$K/linux-3.0.13.tar.xz" https://cdn.kernel.org/pub/linux/kernel/v3.x/linux-3.0.13.tar.xz
  [ -f "$K/bl.tar.bz2" ] || curl -sSfL -o "$K/bl.tar.bz2" https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/$BL.tar.bz2
  printf '%s  %s\n' 51b447f46b71f73024a1b22b4078c37ef72257f464334b2e91f36cee33abf74c "$K/linux-3.0.13.tar.xz" \
                    c8d4d3ca70442652e0e72f57ae6e878375640508f1e08de3152f63414c43b2e4 "$K/bl.tar.bz2" | sha256sum -c --quiet -
  [ -d "$K/$BL" ] || tar -C "$K" -xjf "$K/bl.tar.bz2"
  if [ ! -f "$K/linux-3.0.13/.config" ]; then
    tar -C "$K" -xJf "$K/linux-3.0.13.tar.xz"
    (cd "$K/linux-3.0.13" && export PATH=$K/$BL/bin:$PATH
     cp include/linux/compiler-gcc4.h include/linux/compiler-gcc7.h
     # The TV's PHYS_OFFSET is 0x0 (verified: _text va 0xc0029000 -> phys 0x29000),
     # not vexpress's 0x60000000. With CONFIG_ARM_PATCH_PHYS_VIRT off, PHYS_OFFSET is
     # a compile-time constant, so a module's virt_to_phys() would be wrong by
     # 0x60000000 unless we fix it here. Only affects phys<->virt arithmetic, not ABI.
     sed -i 's/UL(0x60000000)/UL(0x00000000)/' arch/arm/mach-vexpress/include/mach/memory.h
     make ARCH=arm CROSS_COMPILE=arm-linux- vexpress_defconfig > /dev/null
     # match the TV: "3.0.13 SMP preempt mod_unload ARMv7", 3G/1G split
     ./scripts/config --enable PREEMPT --disable PREEMPT_NONE --disable PREEMPT_VOLUNTARY --enable MODULES \
       --enable MODULE_UNLOAD --disable MODVERSIONS --enable SMP --set-str LOCALVERSION "" --disable LOCALVERSION_AUTO \
       --enable AEABI --disable OABI_COMPAT --enable VFP --enable NEON --enable INPUT_JOYSTICK --enable INPUT_JOYDEV \
       --enable INPUT_EVDEV --enable USB --enable USB_SUPPORT --enable VMSPLIT_3G --disable VMSPLIT_2G --disable VMSPLIT_1G
     make ARCH=arm CROSS_COMPILE=arm-linux- oldnoconfig > /dev/null 2>&1
     make ARCH=arm CROSS_COMPILE=arm-linux- KCFLAGS="-fno-pie -fno-pic -Wno-error" modules_prepare > /dev/null 2>&1)
  fi
  make -C "$TOP/src/kmod" > /dev/null
  ls -la "$TOP/src/kmod/"*.ko
}

step_retroarch() {
  log "RetroArch + libretro cores"
  local R=$TP/retro D=$OUT/retroarch
  gitpin https://github.com/libretro/RetroArch.git         "$R/RetroArch"         69a4f0ea1e8aaf442ae4858f2e7f2b31a1776576
  gitpin https://github.com/libretro/libretro-fceumm.git   "$R/libretro-fceumm"   77468e568d9fe9c48d684ca911f1a3fe7259fdf7
  gitpin https://github.com/libretro/snes9x2005.git        "$R/snes9x2005"        a79dfe9047e7fec58808aefe48ad2bf499c7af11
  gitpin https://github.com/libretro/gambatte-libretro.git "$R/gambatte-libretro" 77f2567d718b8973d02caa2daa8dce4953faf811
  gitpin https://github.com/libretro/Genesis-Plus-GX.git   "$R/Genesis-Plus-GX"   b9c2316441978d4015ba30e9a95d9b0e6f8ee981
  gitpin https://github.com/libretro/gpsp.git              "$R/gpsp"              5819380c2ffb0900219d700a382ee68c464ebb99
  gitpin https://github.com/libretro/pcsx_rearmed.git      "$R/pcsx_rearmed"      7b0e8f8fadfbce3d170d195b9d9eecaabfa08c8e
  gitpin https://github.com/libretro/libretro-core-info.git "$R/libretro-core-info" eac3a2784d428444aa2bd472039b5c5540141af1
  # The TV has no libstdc++.so new enough: link it statically.
  sed -i 's/^   LIBS += -lstdc++$/   LIBS += -l:libstdc++.a/' "$R/RetroArch/Makefile.common"
  make -C "$TOP/src" out/stubs/.ok > /dev/null   # EGL/GLES2 link stubs (ra-env.sh)
  (cd "$R/RetroArch" && . ../ra-env.sh && ./configure --host=arm-lgtv-linux-gnueabi \
     --disable-x11 --disable-wayland --disable-kms --disable-vulkan --disable-opengl1 --disable-opengl_core \
     --enable-opengles --disable-opengles3 --enable-sdl2 --disable-sdl --disable-ffmpeg --disable-qt \
     --disable-pulse --disable-alsa --disable-jack --disable-oss --disable-tinyalsa --disable-udev \
     --disable-discord --disable-pipewire --disable-libusb --disable-dbus --disable-v4l2 --disable-mpv \
     --disable-ssl --disable-builtinmbedtls --disable-videocore --disable-mali_fbdev --disable-vivante_fbdev \
     --enable-egl --disable-xinerama --disable-freetype --disable-caca --disable-sixel > configure.log 2>&1 \
     || { tail -20 configure.log; exit 1; }
   make -j"$J" LINK="${X}g++" LDFLAGS="$LDFLAGS" > build.log 2>&1 || { grep -E " error|undefined" build.log | head; exit 1; })
  "$R/build-cores.sh"
  mkdir -p "$D/cores" "$D/info" "$D/roms/240p Test Suite" "$D/system" "$D/saves" "$D/states"
  "${X}strip" -o "$D/retroarch" "$R/RetroArch/retroarch"
  for c in fceumm snes9x2005 gambatte gpsp genesis_plus_gx pcsx_rearmed; do
    "${X}strip" -o "$D/cores/${c}_libretro.so" "$R/cores/${c}_libretro.so"
    cp "$R/libretro-core-info/${c}_libretro.info" "$D/info/"
  done
  # 240p Test Suite (GPL-2.0) as a ready-to-run test ROM set
  local T="$D/roms/240p Test Suite"
  fetch https://github.com/pinobatch/240p-test-mini/releases/download/v0.23/240pee.nes 240pee.nes \
        04f01d7372f66ea2befe325b9bd655a9fcc395a31fa46d9466286bb8f9d2e62e
  fetch https://github.com/pinobatch/240p-test-mini/releases/download/v0.23/gb240p.gb gb240p.gb \
        733eb6e7ec0719752ddd5fcda4e715c2f2e20f4ac4bc971c4bd1d49272f9bb04
  fetch https://github.com/pinobatch/240p-test-mini/releases/download/v0.23/240pee_mb.gba 240pee_mb.gba \
        47844f7140738a06f8f3bc09780da3ab095539a250b870f614feed561d9d6f34
  cp "$DL/240pee.nes" "$DL/gb240p.gb" "$DL/240pee_mb.gba" "$T/"
  fetch https://raw.githubusercontent.com/pinobatch/240p-test-mini/v0.23/LICENSE 240p-LICENSE.txt \
        8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643
  cp "$DL/240p-LICENSE.txt" "$T/LICENSE.txt"
}

STEPS="sdl2 mixer ttf smb2 vterm lvgl doom tyrian rdp retroarch ffmpeg"
[ $# -gt 0 ] && STEPS="$*"
for s in $STEPS; do "step_$s"; done
log "done: $STEPS"
