#!/bin/sh
# Launch a libnc app with the environment procmgr gives LG apps (copied from a running fxui),
# minus LG's crash/debug hooks, with core dumps off and cwd in RAM.
#   run-app.sh <app-binary> [--service lg.service.normal.NAME] [app args...]
APP=$1; shift
[ -x "$APP" ] || { echo "usage: run-app.sh <app> [--service NAME] [args]" >&2; exit 1; }
case "$APP" in /*) ;; *) APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")";; esac
SVC=lg.service.normal.$(basename "$APP")
if [ "$1" = "--service" ]; then SVC=$2; shift 2; fi
cd /tmp || exit 1
ulimit -c 0
SRC=$(pidof fxui | awk '{print $NF}')
[ -n "$SRC" ] || { echo "run-app.sh: no fxui process to copy the environment from" >&2; exit 1; }
tr '\0' '\n' < /proc/$SRC/environ \
  | grep -vE '^(PM_|HOOK_|CRASH_FRAMEWORK|GLIBC_BACKTRACE|LOAD_SYM|LOAD_DSYM|GDB_ROOT|LOCKUP_LEVEL|PWD|APP_START|ATTR_|USE_OSAMM|OSAMEM_SIZE)' \
  | sed "s/'/'\\\\''/g; s/^\([A-Za-z_][A-Za-z0-9_]*\)=\(.*\)\$/export \1='\2'/" > /tmp/ncapp.env
. /tmp/ncapp.env
# HOA_PROC_RegisterService parses PM_EXECARGS (not argv) when it is set.
export PM_EXECPID=$(pidof procmgr) PM_SHELLPID=$$ PM_EXECPATH="$APP" PM_EXECARGS="--service $SVC"
# LG's environment has HOME=/home, which is on the TV's flash: keep all app state on the USB stick.
NCHOME="$(cd "$(dirname "$0")/.." && pwd)/home"
mkdir -p "$NCHOME"
# The one settings file, next to the netcast/ folder (see system/ncconf.sh)
export NETCAST_CONF="${NETCAST_CONF:-$(cd "$(dirname "$0")/../.." && pwd)/netcast.conf}"
export HOME="$NCHOME" XDG_DATA_HOME="$NCHOME/.local/share" XDG_CONFIG_HOME="$NCHOME/.config" XDG_CACHE_HOME="/tmp/ncapp-cache"
# libafopenapi maps $APPFRWK_MASK_PATH (on flash) read-write/shared in every app: use a RAM copy.
[ -f /tmp/appfrwk_mask ] || cp /mnt/lg/cmn_data/appfrwk_mask /tmp/appfrwk_mask 2>/dev/null
export APPFRWK_MASK_PATH=/tmp/appfrwk_mask
# SDL gamepads: hide LG's virtual input devices (LGE CURSOR RETURNPATH, LG Magic gamepad; both 0001:0001),
# which SDL would otherwise report as game controllers ahead of a real pad.
export SDL_GAMECONTROLLER_IGNORE_DEVICES=0x0001/0x0001
# GStreamer (SDL audio) would rewrite its plugin registry on flash: use a RAM copy instead.
[ -f /tmp/ncapp-gstreg.bin ] || cp /mnt/flash/data/gst_registry.arm.bin /tmp/ncapp-gstreg.bin 2>/dev/null
export GST_REGISTRY=/tmp/ncapp-gstreg.bin GST_REGISTRY_UPDATE=no
# App-private libs (e.g. from the buildroot SDK) next to the binary, searched last.
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$(dirname "$APP")/lib"
exec "$APP" "$@"
