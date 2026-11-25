#!/bin/bash
# apitest.sh: exercise every endpoint of the ncplus remote API (tools/ncapi).
#
#   tools/apitest.sh [--keys] [--launch] [-t TOKEN] [-u http://TV:9080]
#       against the TV. TOKEN defaults to $NCAPI_TOKEN, else it is read over telnet (ncapi token show);
#       the address defaults to tools/tv.py's TV on port 9080. Read-only apart from settings writes that
#       keep the current value. --keys also sends volup + voldown; --launch also opens and leaves the
#       launcher (changes what is on screen).
#   tools/apitest.sh --local
#       builds ncapi for the host (make -C src host-ncapi) and runs it against a fake netcast tree in a
#       temp dir (fake key device, stub ncscreen and start.sh session loop), including key injection,
#       settings writes and app switching.
#   tools/apitest.sh --local --run 'CMD'
#       the same server and fake tree, but runs CMD against it instead of the checks (exit status = CMD's),
#       with NCAPI_URL, NCAPI_TOKEN, NCAPI_LOCAL=1, NCAPI_KEYDEV (injected events) and NCAPI_CONF set.
#       tools/hatest.sh uses it to run the Home Assistant tests against the real ncapi.
set -uo pipefail
S=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$S/.." && pwd)
LOCAL=0 KEYS=0 LAUNCH=0 TOKEN=${NCAPI_TOKEN:-} URL= RUN=
while [ $# -gt 0 ]; do
  case "$1" in
    --local) LOCAL=1 ;;
    --keys) KEYS=1 ;;
    --launch) LAUNCH=1 ;;
    -t) TOKEN=$2; shift ;;
    -u) URL=$2; shift ;;
    --run) RUN=$2; shift ;;
    *) sed -n '2,17p' "$0"; exit 2 ;;
  esac
  shift
done

PASS=0 FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   $*"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $*"; }
# req METHOD PATH [BODY] [TOKEN]: sets CODE and BODY
req() {
  local tok=${4-$TOKEN} out
  local args=(-sS -m 10 -o - -w '\n%{http_code}' -X "$1")
  [ -n "$tok" ] && args+=(-H "Authorization: Bearer $tok")
  [ -n "${3:-}" ] && args+=(-H 'Content-Type: application/json' --data-binary "$3")
  out=$(curl "${args[@]}" "$URL$2" 2>&1)
  CODE=${out##*$'\n'}
  BODY=${out%$'\n'*}
}
expect() {   # expect CODE PATTERN DESCRIPTION
  if [ "$CODE" = "$1" ] && printf '%s' "$BODY" | grep -qE -- "$2"; then ok "$3"
  else bad "$3 (got $CODE: $BODY)"; fi
}
wait_for() {   # wait_for SECONDS PATTERN DESCRIPTION: poll /status until it matches
  local i
  for i in $(seq 1 $(($1 * 5))); do
    req GET /api/v1/status
    printf '%s' "$BODY" | grep -qE -- "$2" && { ok "$3"; return; }
    sleep 0.2
  done
  bad "$3 (status: $BODY)"
}

if [ "$LOCAL" = 1 ]; then
  make -s -C "$TOP/src" host-ncapi || exit 1
  BIN=$TOP/src/out/host/ncapi
  D=$(mktemp -d "${TMPDIR:-/tmp}/apitest.XXXXXX")
  trap 'kill $SRV 2>/dev/null; pkill -f "$D/netcast/start.sh" 2>/dev/null; rm -rf "$D"' EXIT
  NC=$D/netcast
  mkdir -p "$NC/apps/doom" "$NC/apps/music" "$NC/apps/broken" "$NC/tools" "$NC/system" "$D/run"
  printf 'title=Doom\ndesc=Freedoom "Phase 1"\nexec=doom.sh\norder=10\n' > "$NC/apps/doom/app.ini"
  printf 'title=Music\r\ndesc=FLAC\r\nexec=run.sh\r\norder=27\r\n' > "$NC/apps/music/app.ini"
  printf 'title=No exec\n' > "$NC/apps/broken/app.ini"
  echo test-1 > "$NC/system/VERSION"
  # stub tools: record what was run
  for t in ncscreen ncnosleep ncnetkeep nclite; do
    printf '#!/bin/sh\necho "%s $*" >> "%s/run/tools.log"\n' "$t" "$D" > "$NC/tools/$t"
  done
  cat >> "$NC/tools/ncscreen" <<EOF
case "\$1" in on|off) echo "\$1" > "$D/run/ncscreen.state";; esac
[ "\$2" = --wake ] && { trap 'kill \$!; exit 0' TERM; sleep 300 & wait; }
exit 0
EOF
  printf '#!/bin/sh\necho "run-app $*" >> "%s/run/tools.log"\n' "$D" > "$NC/apps/run-app.sh"
  # stub session loop: same protocol as the real start.sh (ncsession.pid, ncmenu.cmd/current, ncapp.fg)
  cat > "$NC/start.sh" <<'EOF'
#!/bin/sh
R=$NC_RUNDIR
echo $$ > "$R/ncsession.pid"
echo started >> "$R/start.log"
while :; do
  if [ -f "$R/ncmenu.cmd" ]; then C=$(cat "$R/ncmenu.cmd"); rm -f "$R/ncmenu.cmd"; else C=launcher; fi
  [ "$C" = exit ] && break
  echo "${C##*/}" > "$R/ncmenu.current"
  sh -c 'echo "$$ $(cut -d" " -f22 /proc/$$/stat)" > "$0"; exec sleep 300' "$R/ncapp.fg"
  rm -f "$R/ncapp.fg"
done
rm -f "$R/ncsession.pid" "$R/ncmenu.current"
EOF
  chmod +x "$NC/tools/"* "$NC/start.sh"
  CONF=$D/netcast.conf
  printf '# my settings\n[settings]\nSAVER_MIN=20\r\nNOSLEEP=1\n\n[photos]\nurl=http://x/\n' > "$CONF"
  : > "$D/keydev"
  export NETCAST_DIR=$NC NETCAST_CONF=$CONF NC_RUNDIR=$D/run NC_KEYDEV=$D/keydev
  printf '\n[api]\nPORT=19080' >> "$CONF"   # no newline at the end: the token must go on its own line
  "$BIN" token show >/dev/null && bad "token show without a token" || ok "token show fails without a token"
  "$BIN" > "$D/run/ncapi.log" 2>&1; [ $? = 1 ] && grep -q 'no token' "$D/run/ncapi.log" && ok "server refuses to start without a token" \
    || bad "server without token: $(cat "$D/run/ncapi.log")"
  "$BIN" token ensure && TOKEN=$("$BIN" token show)
  [[ "$TOKEN" =~ ^[0-9a-f]{32}$ ]] && ok "token ensure: $TOKEN" || bad "token ensure: '$TOKEN'"
  "$BIN" token ensure; [ "$("$BIN" token show)" = "$TOKEN" ] && ok "token ensure keeps the token" || bad "token ensure changed it"
  "$BIN" >> "$D/run/ncapi.log" 2>&1 &
  SRV=$!
  URL=http://127.0.0.1:19080
  for i in $(seq 50); do curl -s -o /dev/null "$URL/" && break; sleep 0.1; done
else
  if [ -z "$URL" ]; then
    TV=$(python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import tv; print(tv.tv_host())' "$S") || exit 1
    URL=http://$TV:9080
  fi
  if [ -z "$TOKEN" ]; then
    TOKEN=$(python3 -I "$S/tv.py" 'sh -c "$(ls /mnt/usb/usb*/Drive*/netcast/tools/ncapi | head -1) token show"' | tr -d '\r' | tail -1)
  fi
  [ -n "$TOKEN" ] || { echo "no token (-t TOKEN or NCAPI_TOKEN)"; exit 1; }
fi
echo "API $URL"
if [ -n "$RUN" ]; then
  [ "$LOCAL" = 1 ] || { echo "--run needs --local"; exit 2; }
  NCAPI_URL=$URL NCAPI_TOKEN=$TOKEN NCAPI_LOCAL=1 NCAPI_KEYDEV=$D/keydev NCAPI_CONF=$CONF sh -c "$RUN"
  RC=$?
  [ "$RC" = 0 ] || { echo "--- ncapi log"; cat "$D/run/ncapi.log"; }
  exit $RC
fi

echo "auth and errors"
req GET /api/v1/status "" ""; expect 401 'token' "no token -> 401"
req GET /api/v1/status "" "0123456789abcdef0123456789abcdef"; expect 401 'token' "wrong token -> 401"
req GET /api/v1/status "" "${TOKEN}x"; expect 401 'token' "token with extra char -> 401"
req GET /api/v1/nothing; expect 404 'not found' "unknown path -> 404"
req GET /; expect 404 'api/v1' "/ -> 404"
req GET /api/v1/key; expect 405 'method' "GET /key -> 405"
req POST /api/v1/key '{"key":'; expect 400 'JSON' "broken JSON -> 400"
req POST /api/v1/key '{"key":{"a":1}}'; expect 400 'JSON' "nested JSON -> 400"

echo "status, keys, apps"
req GET /api/v1/status; expect 200 '"screen":"(on|off)"' "status"
echo "       $BODY"
printf '%s' "$BODY" | grep -qE '"id":"([0-9a-f]{2}:){5}[0-9a-f]{2}"' && ok "status has a MAC id" || bad "status id: $BODY"
req GET /api/v1/keys; expect 200 '"name":"volup","code":115' "keys"
req GET /api/v1/apps; expect 200 '"apps":\[' "apps"
[ "$LOCAL" = 1 ] && { printf '%s' "$BODY" | grep -q '^{"apps":\[{"id":"doom","title":"Doom","desc":"Freedoom \\"Phase 1\\"","order":10},{"id":"music","title":"Music","desc":"FLAC","order":27}\]}$' \
  && ok "apps: sorted, CRLF stripped, quotes escaped, no-exec skipped" || bad "apps body: $BODY"; }

echo "keys"
req POST /api/v1/key '{"code":897}'; expect 403 'IN-STOP' "code 897 refused"
req POST /api/v1/key '{"code":898}'; expect 403 'IN-STOP' "code 898 refused"
req POST /api/v1/key '{"key":"nosuchkey"}'; expect 400 'unknown key' "unknown key name -> 400"
req POST /api/v1/key '{"code":1.5}'; expect 400 'integer' "non-integer code -> 400"
req POST /api/v1/key '{"key":"up","repeat":1000}'; expect 400 'repeat' "repeat out of range -> 400"
req POST /api/v1/key '{}'; expect 400 'need' "no key -> 400"
if [ "$LOCAL" = 1 ]; then
  req POST /api/v1/key '{"key":"VolUp"}'; expect 202 '"code":115' "key volup (case-insensitive)"
  req POST /api/v1/key '{"code":28,"repeat":2,"hold_ms":50,"delay_ms":50}'; expect 202 '"name":"ok"' "code 28 x2"
  sleep 0.6
  EV=$(python3 -I -c 'import struct,sys
d=open(sys.argv[1],"rb").read(); n=struct.calcsize("llHHi")
print(" ".join("%d:%d:%d"%struct.unpack_from("llHHi",d,i)[2:] for i in range(0,len(d),n)))' "$D/keydev")
  [ "$EV" = "1:115:1 0:0:0 1:115:0 0:0:0 1:28:1 0:0:0 1:28:0 0:0:0 1:28:1 0:0:0 1:28:0 0:0:0" ] \
    && ok "injected events: press/SYN/release/SYN in order" || bad "injected events: $EV"
elif [ "$KEYS" = 1 ]; then
  req POST /api/v1/key '{"key":"volup"}'; expect 202 '"code":115' "key volup"
  sleep 1
  req POST /api/v1/key '{"key":"voldown"}'; expect 202 '"code":114' "key voldown"
fi

echo "settings"
req GET /api/v1/settings; expect 200 '"SAVER_MIN":\{"value":[0-9]+' "settings"
CUR=$(printf '%s' "$BODY" | grep -oE '"NETKEEP":\{"value":[01]' | grep -oE '[01]$')
req PUT /api/v1/settings "{\"NETKEEP\":$CUR}"; expect 200 "\"NETKEEP\":\\{\"value\":$CUR" "PUT unchanged NETKEEP=$CUR"
req PUT /api/v1/settings '{"SAVER_MIN":7}'; expect 400 'one of 0 5 10 20 30 60' "SAVER_MIN=7 -> 400"
req PUT /api/v1/settings '{"SAVER_MIN":"20"}'; expect 400 'one of' "string value -> 400"
req PUT /api/v1/settings '{"API":0}'; expect 403 'on the TV' "API is read-only -> 403"
req PUT /api/v1/settings '{"BOGUS":1}'; expect 400 'unknown setting' "unknown setting -> 400"
req PUT /api/v1/settings '{"NETKEEP":1,"SAVER_MIN":7}'; expect 400 'SAVER_MIN' "one bad value rejects the whole PUT"
req POST /api/v1/settings '{}'; expect 405 'method' "POST /settings -> 405"
if [ "$LOCAL" = 1 ]; then
  req PUT /api/v1/settings '{"SAVER_MIN":30,"LITE":true,"NETKEEP":0}'; expect 200 '"SAVER_MIN":\{"value":30' "PUT SAVER_MIN=30 LITE=1 NETKEEP=0"
  sleep 0.3
  WANT=$(printf '# my settings\n[settings]\nSAVER_MIN=30\r\nNOSLEEP=1\nLITE=1\nNETKEEP=0\n\n[photos]\nurl=http://x/\n\n[api]\nPORT=19080\nTOKEN=%s\n' "$TOKEN")
  [ "$(cat "$CONF")" = "$WANT" ] && ok "netcast.conf: only those keys changed, rest kept" || { bad "netcast.conf:"; cat -A "$CONF"; }
  grep -q 'ncscreen saver 30' "$D/run/tools.log" && grep -q 'run-app .*nclite stop' "$D/run/tools.log" \
    && ok "applied: saver restarted, nclite stop" || bad "applied: $(cat "$D/run/tools.log")"
fi

echo "screen"
req POST /api/v1/screen '{"on":"no"}'; expect 400 'need' "screen with a string -> 400"
if [ "$LOCAL" = 1 ]; then
  req POST /api/v1/screen '{"on":false}'; expect 202 '"screen":"off"' "screen off"
  sleep 0.3; req GET /api/v1/status; expect 200 '"screen":"off"' "status: screen off"
  pgrep -f "$NC/tools/ncscreen off --wake" >/dev/null && ok "ncscreen off --wake is waiting for a key" || bad "no ncscreen waiter"
  req POST /api/v1/screen '{"on":true}'; expect 202 '"screen":"on"' "screen on"
  sleep 0.3; req GET /api/v1/status; expect 200 '"screen":"on"' "status: screen on"
  pgrep -f "$NC/tools/ncscreen off --wake" >/dev/null && bad "ncscreen waiter still running" || ok "waiter stopped"
fi

echo "launch"
req POST /api/v1/launch '{"app":"../x"}'; expect 404 'no such app' "launch ../x -> 404"
req POST /api/v1/launch '{"app":"broken"}'; expect 404 'no such app' "launch an app without exec= -> 404"
req POST /api/v1/launch '{}'; expect 400 'need' "launch without app -> 400"
if [ "$LOCAL" = 1 ]; then
  req POST /api/v1/launch '{"app":"exit"}'; expect 202 'not running' "exit while not running: no-op"
  req POST /api/v1/launch '{"app":"doom"}'; expect 202 'starting' "launch doom: starts ncplus"
  wait_for 5 '"netcast":true,"app":"doom","title":"Doom"' "status: doom in front"
  req POST /api/v1/launch '{"app":"music"}'; expect 202 'switching' "launch music while doom runs"
  wait_for 5 '"app":"music","title":"Music"' "status: music in front"
  req POST /api/v1/launch '{"app":"launcher"}'; expect 202 'switching' "launch launcher"
  wait_for 5 '"app":"launcher","title":"ncplus"' "status: launcher"
  req POST /api/v1/launch '{"app":"exit"}'; expect 202 'switching' "exit to LG"
  wait_for 5 '"netcast":false,"app":"LG"' "status: LG"
  [ "$(grep -c started "$D/run/start.log")" = 1 ] && ok "start.sh started once" || bad "start.log: $(cat "$D/run/start.log")"
elif [ "$LAUNCH" = 1 ]; then
  req POST /api/v1/launch '{"app":"launcher"}'; expect 202 'ok' "launch launcher"
  wait_for 20 '"app":"launcher"' "status: launcher"
  req POST /api/v1/launch '{"app":"exit"}'; expect 202 'ok' "exit to LG"
  wait_for 10 '"app":"LG"' "status: LG"
fi

echo "limits"
req POST /api/v1/key "{\"key\":\"$(head -c 9000 /dev/zero | tr '\0' a)\"}"; expect 413 'too large' "9 KB body -> 413"

echo "$PASS passed, $FAIL failed"
[ "$LOCAL" = 1 ] && [ "$FAIL" -gt 0 ] && { echo "--- ncapi log"; cat "$D/run/ncapi.log"; }
[ "$FAIL" = 0 ]
