#!/bin/sh
# OpenTyrian. Remote: arrows move, OK fire / select, CH- menu/back,
#                     RED change rear weapon, GREEN/YELLOW sidekicks.
D=$(cd "$(dirname "$0")" && pwd)
CFG="$D/../../home/.config/opentyrian"
mkdir -p "$CFG"
[ -f "$CFG/opentyrian.cfg" ] || cp "$D/opentyrian.cfg" "$CFG/opentyrian.cfg"
exec sh "$D/../run-app.sh" "$D/opentyrian" --data="$D/data" "$@"
