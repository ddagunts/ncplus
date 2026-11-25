#!/bin/sh
# Terminal: the ncplus chroot shell (if the image is on the stick), else the TV's shell.
D=$(cd "$(dirname "$0")" && pwd)
exec sh "$D/../run-app.sh" "$D/ncterm" "$@"
