#!/bin/sh
# Web Browser (NetSurf 3.11). Start page: a URL argument, else home= in the [browser] section of netcast.conf,
# else the local test pages (tools/webtest.sh: BusyBox httpd on 127.0.0.1:8090, stopped again on exit).
# Remote: arrows move the pointer, OK clicks, push against the top/bottom edge to scroll,
#         BACK = previous page (quits on the first page), GREEN = pointer/text mode (USB keyboard typing).
D=$(cd "$(dirname "$0")" && pwd); N=$(cd "$D/../.." && pwd)
. "$N/system/ncconf.sh"; ncconf_init
URL=${1:-$(ncconf_get browser home)}
STARTED=0
if [ -z "$URL" ] && ! sh "$N/tools/webtest.sh" status >/dev/null 2>&1; then
  sh "$N/tools/webtest.sh" start && STARTED=1
fi
if [ -z "$URL" ] && sh "$N/tools/webtest.sh" status >/dev/null 2>&1; then URL=http://127.0.0.1:8090/; fi
export NETSURFRES="$D/res"
# NetSurf only takes --option=value arguments before all others
sh "$D/../run-app.sh" "$D/netsurf" "--ca_bundle=$D/res/ca-bundle.crt" -w 1280 -h 720 ${URL:+"$URL"}
rc=$?
[ "$STARTED" = 1 ] && sh "$N/tools/webtest.sh" stop
exit $rc
