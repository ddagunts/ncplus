#!/bin/sh
# Fetch Qt 6.12.0 (qtbase, qtshadertools, qtdeclarative) into src/ and add the eglfs_netcast plugin.
# Then: ./build-host.sh && ./build-target.sh
set -e
Q=$(cd "$(dirname "$0")" && pwd)
V=6.12.0
mkdir -p "$Q/dl" "$Q/src"
while read -r sum m; do
  f=$m-everywhere-src-$V.tar.xz
  [ -f "$Q/dl/$f" ] || curl -sSfL -o "$Q/dl/$f" "https://download.qt.io/official_releases/qt/6.12/$V/submodules/$f"
  echo "$sum  $Q/dl/$f" | sha256sum -c --quiet -
  if [ ! -d "$Q/src/$m-everywhere-src-$V" ]; then
    tar -C "$Q/src" -xJf "$Q/dl/$f"
    [ "$m" = qtbase ] && patch -d "$Q/src/$m-everywhere-src-$V" -p1 < "$Q/../patches/qtbase-$V-eglfs_netcast.patch"
  fi
done <<SUMS
a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307 qtbase
c7d84f436e1aaef39fdcadebf2bd71bdf24dc69497e13f2e0198873cbbfea2ab qtshadertools
311f3a2603e1973bb59baef9dfa740a376de713157d4782ee043681e889c9260 qtdeclarative
SUMS
echo "Qt $V sources ready in $Q/src"
