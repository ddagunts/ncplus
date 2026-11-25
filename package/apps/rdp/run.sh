#!/bin/sh
# Remote Desktop (FreeRDP 3 SDL2 client). Set the server in the [rdp] section of netcast.conf
# (next to the netcast/ folder): one line of FreeRDP arguments, e.g.  /v:192.168.1.20 /u:me
# Missing user/password are asked for on screen (USB keyboard).
D=$(cd "$(dirname "$0")" && pwd); N=$(cd "$D/../.." && pwd)
. "$N/system/ncconf.sh"; ncconf_init
ARGS=$(ncconf_section rdp | head -1)
[ -n "$ARGS" ] || { echo "rdp: put a server in the [rdp] section of $NETCAST_CONF (/v:HOST /u:USER)"; exit 1; }
# shellcheck disable=SC2086
exec sh "$D/../run-app.sh" "$D/sdl-freerdp" $ARGS /size:1280x720 /f /cert:tofu /bpp:32 /network:auto /gfx /sound:sys:fake +clipboard -grab-keyboard /log-level:WARN
