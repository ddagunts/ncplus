#!/bin/sh
D=$(cd "$(dirname "$0")" && pwd)
exec sh "$D/../run-app.sh" "$D/ncsettings"
