#!/bin/sh
# Static Qt 6.12 (base, shadertools, declarative) for LG NetCast: eglfs + eglfs_netcast, GLES2, evdev.
set -e
Q=$(cd "$(dirname "$0")" && pwd)
TOP=$Q/../..
export PATH=$Q/../cmake-3.31.10-linux-x86_64/bin:$PATH
SDK=$TOP/toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot
T=$Q/target
LINK="-Wl,--allow-shlib-undefined -static-libgcc -static-libstdc++"
mkdir -p $Q/build-target/qtbase && cd $Q/build-target/qtbase
$Q/src/qtbase-everywhere-src-6.12.0/configure -prefix $T -static -release -nomake examples -nomake tests \
  -opengl es2 -egl -feature-eglfs -qpa eglfs -no-widgets -no-dbus -no-glib -no-icu -no-fontconfig \
  -qt-freetype -qt-harfbuzz -qt-zlib -qt-pcre -qt-libpng -no-feature-jpeg \
  -no-feature-sql -no-feature-network -no-feature-printsupport -no-feature-testlib -no-feature-concurrent -no-feature-xml \
  -no-libudev -no-libinput -no-tslib -no-mtdev -feature-evdev -no-xcb -no-feature-wayland -no-feature-vulkan \
  -no-feature-linuxfb -no-feature-kms -no-gbm -no-feature-eglfs_x11 -no-feature-eglfs_mali -no-feature-eglfs_viv \
  -- -DCMAKE_TOOLCHAIN_FILE=$SDK/share/buildroot/toolchainfile.cmake -DQT_HOST_PATH=$Q/host \
  -DNETCAST_LIBNC_DIR=$TOP/src/libnc -DNETCAST_LIBNC_LIB=$TOP/src/out/libnc.a \
  -DEGL_INCLUDE_DIR=$TOP/third_party/khronos -DEGL_LIBRARY=$TOP/src/out/stubs/libEGL.so \
  -DGLESv2_INCLUDE_DIR=$TOP/third_party/khronos -DGLESv2_LIBRARY=$TOP/src/out/stubs/libGLESv2.so \
  -DCMAKE_EXE_LINKER_FLAGS="$LINK" -DCMAKE_SHARED_LINKER_FLAGS="$LINK" \
  -DCMAKE_CXX_STANDARD_LIBRARIES="-lrt -ldl -lpthread" -DCMAKE_C_STANDARD_LIBRARIES="-lrt -ldl -lpthread"
cmake --build . --parallel 6
cmake --install .
for m in qtshadertools qtdeclarative; do
  mkdir -p $Q/build-target/$m && cd $Q/build-target/$m
  $T/bin/qt-configure-module $Q/src/$m-everywhere-src-6.12.0 -- -DQT_FEATURE_qml_network=OFF -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF
  cmake --build . --parallel 6
  cmake --install .
done
echo TARGET-QT-DONE
