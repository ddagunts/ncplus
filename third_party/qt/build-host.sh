#!/bin/sh
# Host Qt 6.12 tools (moc, rcc, qsb, qmlcachegen, ...) needed to cross-compile Qt for the TV.
set -e
Q=$(cd "$(dirname "$0")" && pwd)
export PATH=$Q/../cmake-3.31.10-linux-x86_64/bin:$PATH
H=$Q/host
mkdir -p $Q/build-host/qtbase && cd $Q/build-host/qtbase
$Q/src/qtbase-everywhere-src-6.12.0/configure -prefix $H -release -nomake examples -nomake tests \
  -no-opengl -no-widgets -no-dbus -no-feature-sql -no-feature-network -no-feature-printsupport \
  -no-feature-testlib -no-feature-concurrent -no-feature-xml -no-glib -no-icu -qt-zlib -qt-pcre -qt-freetype -qt-harfbuzz -qt-libpng -no-feature-jpeg \
  -no-xcb -no-feature-wayland -no-feature-vulkan -no-feature-linuxfb -no-feature-eglfs
cmake --build . --parallel 6
cmake --install .
for m in qtshadertools qtdeclarative; do
  mkdir -p $Q/build-host/$m && cd $Q/build-host/$m
  $H/bin/qt-configure-module $Q/src/$m-everywhere-src-6.12.0 -- -DQT_FEATURE_qml_network=OFF -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF
  cmake --build . --parallel 6
  cmake --install .
done
echo HOST-QT-DONE
