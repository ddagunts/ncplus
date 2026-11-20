#!/bin/bash
# Copy LG's glibc 2.12.2 (/lib/*-2.12.2.so) from the TV into tv_dump/glibc/ for build/build-image.sh.
set -euo pipefail
S=$(cd "$(dirname "$0")" && pwd)
D=$S/../tv_dump/glibc
mkdir -p "$D"
for f in $(python3 -I "$S/tv.py" 'ls /lib/*-2.12.2.so' | tr -s ' \t\r' '\n' | grep '^/lib/'); do
  "$S/pull.sh" "$f" "$D"
done
ls "$D"
