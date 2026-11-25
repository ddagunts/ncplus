#!/bin/sh
# RetroArch: put ROMs in roms/ (and BIOS files, e.g. PlayStation, in system/).
#   run.sh                      -> RetroArch menu
#   run.sh -L cores/X.so ROM    -> start a game directly
D=$(cd "$(dirname "$0")" && pwd)
cd "$D"
if [ $# -eq 0 ]; then set -- --menu; fi
exec sh "$D/../run-app.sh" "$D/retroarch" --config "$D/retroarch.cfg" "$@"
