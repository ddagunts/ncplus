#!/bin/sh
# Launch Chocolate Doom (Freedoom: Phase 1) on the NetCast TV. All state stays in this folder (USB).
#   Remote: arrows move/turn, OK fire / menu select, CH+ use/open, BACK menu, CH- quit (OK confirms),
#           RED/GREEN strafe, YELLOW run, BLUE automap, 1-7 weapons.
#   Gamepad (Xbox layout): left stick move/strafe, right stick turn, A fire / menu select, B use / menu back,
#           LT run, LB/RB previous/next weapon, Start menu, View automap.
D=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$D/saves"
exec sh "$D/../run-app.sh" "$D/chocolate-doom" -iwad "$D/freedoom1.wad" \
  -config "$D/default.cfg" -extraconfig "$D/chocolate-doom.cfg" -savedir "$D/saves" "$@"
