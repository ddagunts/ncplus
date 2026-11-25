#!/bin/bash
# audrecon.sh [outdir] -- read-only recon of the stock MTK audio path, for the mainline
# speaker driver. Run with the TV on stock (telnet up). Writes nothing on the TV.
#
#   1. module list, mounts, iomem, interrupts
#   2. kallsyms lines for the audio driver (AUD_/Dsp/Aout/Afifo/Pcm/Ntp...)
#   3. every .ko on the mounted partitions (path + size), then pulls the big ones
#   4. AUD register block 0xf0005000..0xf0005fff, twice (idle), via memscan -d
#      (only with REGS=1: register reads are normally harmless here, but this block is
#       not mapped out yet, so it is opt-in)
# Output goes to tv_dump/audio/ (gitignored: these are LG/MTK binaries and dumps).
set -e
cd "$(dirname "$0")/.."
OUT=${1:-tv_dump/audio}
mkdir -p "$OUT"
T="python3 -I tools/tv.py"

$T 'cat /proc/modules; echo; cat /proc/mounts; echo; cat /proc/iomem; echo; cat /proc/interrupts' 60 \
  | tr -d '\r' > "$OUT/sys.txt"
$T "grep -i -E 'aud|dsp|aout|afifo|pcm|ntp7|amp_|spdif|iec' /proc/kallsyms" 120 \
  | tr -d '\r' > "$OUT/kallsyms-audio.txt"
$T 'cat /proc/kallsyms' 300 | tr -d '\r' > "$OUT/kallsyms.txt"
# The TV's busybox has no sort: dedupe on the host.
$T "for m in \$(cut -d' ' -f2 /proc/mounts); do find \$m -xdev -name '*.ko' 2>/dev/null; done \
    | while read f; do echo \"\$(wc -c < \$f) \$f\"; done" 300 | tr -d '\r' | sort -u > "$OUT/ko-list.txt"
echo "modules loaded:"; cut -d' ' -f1,2 "$OUT/sys.txt" | sed -n '1,/^$/p'
echo "ko files:"; cat "$OUT/ko-list.txt"

# Pull modules over 512 KiB (the MTK driver is several MB) plus anything named like it.
awk '$1 > 524288 || $2 ~ /mtk|mtal|aud|drv/ {print $2}' "$OUT/ko-list.txt" | sort -u | while read -r f; do
  tools/pull.sh "$f" "$OUT/ko" </dev/null
done

if [ "${REGS:-0}" = 1 ]; then
  root=$($T 'ls -d /mnt/usb/usb*/Drive*/netcast 2>/dev/null | head -1' | tr -d '\r' | tail -1)
  for n in 1 2; do
    $T "$root/mainline/memscan -d 0xf0005000 0x1000" 60 | tr -d '\r' > "$OUT/aud-regs-$n.txt"
  done
  diff "$OUT/aud-regs-1.txt" "$OUT/aud-regs-2.txt" > "$OUT/aud-regs.diff" || true
  echo "registers that changed between snapshots:"; grep -c '^>' "$OUT/aud-regs.diff" || true
fi
ls -la "$OUT"
