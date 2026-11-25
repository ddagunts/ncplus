#!/bin/bash
# Log TV reachability transitions (telnet port 23 + ping) with timestamps to logs/tv-reachability.log.
# The TV's address: $NETCAST_TV or tools/.tv-host (see tv.py).
S=$(cd "$(dirname "$0")" && pwd)
TV=$(python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import tv; print(tv.tv_host())' "$S") || exit 1
mkdir -p "$S/../logs"
L=$S/../logs/tv-reachability.log
state=unknown
while true; do
  if timeout 2 bash -c "exec 3<>/dev/tcp/$TV/23" 2>/dev/null; then s=up
  elif ping -c1 -W1 "$TV" >/dev/null 2>&1; then s=ping-only
  else s=down; fi
  if [ "$s" != "$state" ]; then echo "$(date '+%F %T') $s" >> "$L"; state=$s; fi
  sleep 5
done
