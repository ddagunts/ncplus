#!/bin/bash
# pull.sh /remote/path localdir  -- copy file from TV via base64 over telnet (read-only on TV)
T="python3 -I $(cd "$(dirname "$0")" && pwd)/tv.py"
mkdir -p "$2"; out="$2/$(basename "$1")"
$T "base64 '$1'" 300 | tr -d '\r' | base64 -d > "$out" && ls -la "$out"
