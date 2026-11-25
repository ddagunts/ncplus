#!/usr/bin/env python3
"""ramlog.py: read the mainline kernel's pstore/ramoops log back from the stock system.

After a mainline boot ends in a watchdog reset (reboot, or panic=10), DRAM still holds the
ramoops region. This runs `memscan -r` on the TV (from <stick>/netcast/mainline/), pulls the
1 MB over telnet as base64 and prints the zones it finds.

  tools/ramlog.py                 fetch from the TV and decode (also saves ramoops.bin here)
  tools/ramlog.py -f dump.bin     decode a saved dump
  tools/ramlog.py -o out.bin      fetch, save the raw dump to out.bin, decode

Layout must match the ramoops node in src/mainline/mt5398-lg-netcast.dts.
Read-only on the TV.
"""
import base64, os, struct, subprocess, sys

BASE, SIZE = 0x3e000000, 0x100000
RECORD, CONSOLE, PMSG = 0x10000, 0x40000, 0x10000
SIG = 0x43474244  # PERSISTENT_RAM_SIG "DBGC" (every zone of ours uses prz sig 0)

def zones():
    dump = SIZE - CONSOLE - PMSG
    z = [("dmesg-%d" % i, i * RECORD, RECORD) for i in range(dump // RECORD)]
    z.append(("console", dump, CONSOLE))
    z.append(("pmsg", dump + CONSOLE, PMSG))
    return z

def fetch():
    here = os.path.dirname(os.path.abspath(__file__))
    tv = [sys.executable, "-I", os.path.join(here, "tv.py")]
    root = subprocess.run(tv + ["ls -d /mnt/usb/usb*/Drive*/netcast 2>/dev/null | head -1"],
                          capture_output=True, text=True).stdout.strip()
    if not root.startswith("/mnt/usb/"):
        sys.exit("ramlog: no netcast/ folder on a USB stick (got %r)" % root)
    cmd = "%s/mainline/memscan -r 0x%x 0x%x | base64" % (root, BASE, SIZE)
    out = subprocess.run(tv + [cmd, "300"], capture_output=True, text=True).stdout
    raw = base64.b64decode("".join(out.split()))
    if len(raw) != SIZE:
        sys.exit("ramlog: got %d bytes, expected %d" % (len(raw), SIZE))
    return raw

def decode(raw):
    found = 0
    for name, off, zsz in zones():
        sig, start, size = struct.unpack_from("<III", raw, off)
        cap = zsz - 12
        if sig != SIG:
            continue
        if start > cap or size > cap:
            print("== %s @0x%08x: header sig ok but start=%d size=%d > %d (corrupt)" %
                  (name, BASE + off, start, size, cap))
            continue
        if size == 0:
            continue
        data = raw[off + 12: off + 12 + cap]
        # full ring: oldest byte is at start; otherwise the valid bytes are [0, size)
        text = data[start:size] + data[:start] if size == cap else data[:size]
        found += 1
        print("== %s @0x%08x (%d bytes%s)" % (name, BASE + off, size, ", wrapped" if size == cap else ""))
        print(text.decode("utf-8", "replace").rstrip("\n"))
        print()
    if not found:
        print("ramlog: no ramoops zones with data (region never written by mainline, or overwritten)")

def main():
    a = sys.argv[1:]
    if a[:1] == ["-f"]:
        raw = open(a[1], "rb").read()
    else:
        raw = fetch()
        path = a[1] if a[:1] == ["-o"] else "ramoops.bin"
        open(path, "wb").write(raw)
        print("ramlog: saved %s" % path, file=sys.stderr)
    decode(raw)

if __name__ == "__main__":
    main()
