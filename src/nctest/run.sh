#!/bin/sh
# Launch nctest with the environment procmgr gives LG apps (copied from a running fxui),
# minus LG's crash/debug hooks, with core dumps off and cwd in RAM.
D=$(cd "$(dirname "$0")" && pwd)
cd /tmp || exit 1
ulimit -c 0
SRC=$(pidof fxui | awk '{print $NF}')
[ -n "$SRC" ] || { echo "run.sh: no fxui process to copy the environment from" >&2; exit 1; }
tr '\0' '\n' < /proc/$SRC/environ \
  | grep -vE '^(PM_|HOOK_|CRASH_FRAMEWORK|GLIBC_BACKTRACE|LOAD_SYM|LOAD_DSYM|GDB_ROOT|LOCKUP_LEVEL|PWD|APP_START|ATTR_|USE_OSAMM|OSAMEM_SIZE)' \
  | sed "s/'/'\\\\''/g; s/^\([A-Za-z_][A-Za-z0-9_]*\)=\(.*\)\$/export \1='\2'/" > /tmp/nctest.env
. /tmp/nctest.env
# HOA_PROC_RegisterService parses PM_EXECARGS (not argv) when it is set.
SVC=${NCTEST_SERVICE:-lg.service.normal.nctest}
export PM_EXECPID=$(pidof procmgr) PM_SHELLPID=$$ PM_EXECPATH="$D/nctest" PM_EXECARGS="--service $SVC"
exec "$D/nctest" "$@"
