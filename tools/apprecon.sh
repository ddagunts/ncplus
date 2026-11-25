#!/bin/bash
# apprecon.sh [outdir] -- read-only recon of LG's app chooser (stock, telnet up): where the
# tiles come from and how one maps to an executable, so a launcher on the USB stick can get
# a tile without writing flash. Never calls the HOA_APP_AddApp/InstallApp* APIs.
#   1. processes and mounts; open files and maps of appctrl, upctrl and sdpif
#   2. /usr/local/bin/svclist*.rc (service name -> path -> args)
#   3. names only (no contents) of the app/XML trees; flash trees may hold account data
#   4. pulls the appctrl/upctrl binaries found in step 1
#   5. fire.sh's logs, for the "boots only from HOME" failure
# Output: tv_dump/apps/ (gitignored). The TV's busybox has no sort/uniq: filter on the host.
set -e
cd "$(dirname "$0")/.."
OUT=${1:-tv_dump/apps}
mkdir -p "$OUT"
tv() { python3 -I tools/tv.py "$1" "${2:-60}" | tr -d '\r'; }

tv 'ps; echo; cat /proc/mounts' > "$OUT/ps-mounts.txt"
tv 'for p in appctrl upctrl sdpif; do for i in $(pidof $p); do echo "## $p $i"; ls -l /proc/$i/exe; ls -l /proc/$i/fd; cat /proc/$i/maps; done; done' 90 > "$OUT/appctrl-fds.txt"
tv 'for f in /usr/local/bin/svclist*.rc; do echo "## $f"; cat $f; done' 90 > "$OUT/svclist.txt"
tv 'for d in /mnt/lg/res/xml /mnt/addon/contents/system /mnt/appstore /mnt/nsu/base; do find $d 2>/dev/null; done' 200 > "$OUT/app-files.txt"
tv 'tail -60 /tmp/ncboot.log; echo "## fire.log"; for f in /mnt/usb/usb*/Drive*/netcast/mainline/fire.log /mnt/usb/usb*/Drive*/netcast/mainline/nohup.out; do echo "# $f"; tail -60 $f; done' > "$OUT/fire-logs.txt"
grep -oE '/[^ ]+/(appctrl|upctrl|sdpif)$' "$OUT/appctrl-fds.txt" | sort -u | while read -r f; do
	tools/pull.sh "$f" "$OUT/bin" </dev/null
done
ls -la "$OUT"
