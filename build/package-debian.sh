#!/bin/bash
# Build the Debian-for-NetCast USB package: dist/netcast-debian.zip, unzipped onto the root of
# a blank FAT32 stick. It holds the mainline kernel + Debian root and what the stock LG system
# needs to warm-boot into them (nckexec.ko, fire.sh), plus a few tools for the stock system
# (ncpreflight, ncdebug, nckey, evdump, ncnetkeep, memscan), ncrate (sets the panel to 60 Hz), and ncboot + run-app.sh, the splash fire.sh
# puts up when no app is on screen. None of ncplus's apps.
#
#   build/package-debian.sh
#
# Inputs (built beforehand):
#   third_party/build-mainline.sh  -> third_party/mainline/out/{Image, mt5398-lg-netcast.dtb}
#   third_party/build-debian.sh    -> third_party/mainline/out/debian.img (build it without
#                                     NETCAST_SSH_KEY: a release image carries no keys)
#   third_party/build-deps.sh kmod -> src/kmod/nckexec.ko
#   third_party/build-sgx.sh       -> third_party/mainline/out/sgx/ (GPU test: pvrsrvkm.ko,
#                                     sgxgltest, our WSEGL, pvrtest.sh; no LG files)
#   make -C src (the tools below)  -> src/out/
set -euo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD
MO=$TOP/third_party/mainline/out
DIST=$TOP/dist
S=$DIST/netcast-debian
P=$S/netcast
ZIP=$DIST/netcast-debian.zip
TOOLS="ncpreflight ncdebug nckey evdump ncnetkeep memscan ncboot ncrate"
STRIP="$TOP/toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot/bin/arm-lgtv-linux-gnueabi-strip"
KSTRIP="$TOP/third_party/kernel/armv7-eabihf--glibc--stable-2018.11-1/bin/arm-linux-strip"
DEBUGFS=$(command -v debugfs || echo /usr/sbin/debugfs)

make -C src $(for t in $TOOLS; do printf 'out/%s ' "$t"; done) out/run-app.sh >/dev/null
PVR="pvrsrvkm.ko sgxgltest libpvrEWS_NullWSEGL.so pvrtest.sh"
for f in "$MO/Image" "$MO/mt5398-lg-netcast.dtb" "$MO/debian.img" "$TOP/src/kmod/nckexec.ko" \
	$(for f in $PVR; do echo "$MO/sgx/$f"; done); do
	[ -s "$f" ] || { echo "missing $f (see the header of this script)" >&2; exit 1; }
done
# A release image must not carry anyone's SSH key
KEYS=$("$DEBUGFS" -R "cat /root/.ssh/authorized_keys" "$MO/debian.img" 2>/dev/null | grep -c . || true)
if [ "$KEYS" != 0 ]; then
	echo "$MO/debian.img has $KEYS SSH key(s) baked in: rebuild it without NETCAST_SSH_KEY" >&2
	exit 1
fi

rm -rf "$S" "$ZIP"
mkdir -p "$P/mainline" "$P/tools" "$P/LICENSES"
cp "$TOP/src/mainline/fire.sh" "$MO/Image" "$MO/mt5398-lg-netcast.dtb" \
	"$MO/debian.img" "$P/mainline/"
"$KSTRIP" --strip-debug -o "$P/mainline/nckexec.ko" "$TOP/src/kmod/nckexec.ko"
for t in $TOOLS; do cp "$TOP/src/out/$t" "$P/tools/" && "$STRIP" "$P/tools/$t"; done
cp "$TOP/src/out/run-app.sh" "$P/tools/"
# GPU (SGX544) test files. LG's own GPU libraries are not in the package: fire.sh copies
# them from the TV into mainline/pvr/lib.
mkdir -p "$P/mainline/pvr"
for f in $PVR; do cp "$MO/sgx/$f" "$P/mainline/pvr/"; done
"$STRIP" "$P/mainline/pvr/sgxgltest" "$P/mainline/pvr/libpvrEWS_NullWSEGL.so"
# examples only: a user's own authorized_keys and wifi.conf survive unzipping a newer package
cp "$TOP"/src/mainline/stick/*.example "$P/mainline/"
VERSION=$(git describe --always --dirty 2>/dev/null || echo unknown)
echo "$VERSION" > "$P/VERSION"

# --- licences, and the source of every binary ---
cp "$TOP/LICENSE" "$P/LICENSES/ncplus-MIT.txt"
L=$TOP/third_party/mainline/linux-6.12.119
cat "$L/COPYING" "$L/LICENSES/preferred/GPL-2.0" "$L/LICENSES/exceptions/Linux-syscall-note" \
	> "$P/LICENSES/Linux-GPL-2.0.txt"
cp "$TOP/build/busybox-1.36.1/LICENSE" "$P/LICENSES/BusyBox-GPL-2.0.txt"
cp /usr/share/common-licenses/LGPL-2.1 "$P/LICENSES/glibc-LGPL-2.1.txt"
cp "$TOP/third_party/mainline/firmware/wireless-regdb-LICENSE" "$P/LICENSES/wireless-regdb-ISC.txt"
cat "$TOP/third_party/sgx-km/MIT-COPYING" > "$P/LICENSES/PowerVR-DDK-kernel-MIT.txt"
"$DEBUGFS" -R "cat /var/lib/dpkg/status" "$MO/debian.img" 2>/dev/null | python3 -I -c '
import sys
pkgs, cur = [], {}
for line in sys.stdin.read().splitlines() + [""]:
    if not line:
        if cur.get("Status", "").endswith(" installed"):
            src = cur.get("Source", cur["Package"]).split(" ")
            pkgs.append((cur["Package"], cur["Version"], src[0], src[1].strip("()") if len(src) > 1 else cur["Version"]))
        cur = {}
    elif not line[0].isspace() and ":" in line:
        k, v = line.split(":", 1)
        cur[k] = v.strip()
print("Debian packages in mainline/debian.img (binary package, version, source package, source version).")
print("The sources: https://snapshot.debian.org/package/<source>/<source version>/ or apt-get source.")
print()
for p in sorted(pkgs):
    print("%-40s %-32s %s %s" % p)
' > "$P/LICENSES/debian-packages.txt"
cat > "$P/LICENSES/NOTICE.txt" <<EOF
Source code for everything in this package: https://github.com/ddagunts/ncplus (version $VERSION).

mainline/Image   Linux 6.12.119 (GPL-2.0, Linux-GPL-2.0.txt) from kernel.org, with this project's
                 MT5398 drivers (src/mainline/kernel/), patches (src/mainline/patches/) and config
                 (src/mainline/mt5398.config); built by third_party/build-mainline.sh. Its built-in
                 initramfs holds a static BusyBox 1.36.1 (GPL-2.0, BusyBox-GPL-2.0.txt) and
                 src/mainline/initramfs/init. Built-in firmware: carl9170-1.fw 1.9.6 (USB Wi-Fi,
                 GPL-2.0-or-later, Linux-GPL-2.0.txt; source https://github.com/chunkeey/carl9170fw)
                 from linux-firmware, and regulatory.db(.p7s) from wireless-regdb 2026.09.03
                 (ISC, wireless-regdb-ISC.txt).
mainline/*.dtb   src/mainline/mt5398-lg-netcast.dts (GPL-2.0).
mainline/nckexec.ko
                 src/kmod/nckexec.c (GPL-2.0), built against LG's GPL source of the TV's
                 kernel (third_party/build-deps.sh kmod).
mainline/debian.img
                 Debian 13 "trixie" armhf, unmodified Debian packages (each under its own
                 licence: /usr/share/doc/*/copyright in the image; list and versions in
                 debian-packages.txt) and this project's configuration
                 (src/mainline/debian/overlay/, and panel-volume.sh beside it, which adds the volume
                 control to /etc/xdg/xfce4/panel/default.xml); built by third_party/build-debian.sh.
mainline/pvr/pvrsrvkm.ko
                 The PowerVR SGX services kernel module: Imagination Technologies' DDK 1.9 as
                 released by Texas Instruments (git.ti.com/graphics/omap5-sgx-ddk-linux, tag
                 TI_LINUX_OMAP_SGX_DDK_1.9_2253347), Dual MIT/GPL-2.0
                 (PowerVR-DDK-kernel-MIT.txt, Linux-GPL-2.0.txt), with this project's patches
                 and MT5398 system layer (src/mainline/sgx/); built by third_party/build-sgx.sh.
mainline/pvr/sgxgltest, mainline/pvr/libpvrEWS_NullWSEGL.so, mainline/pvr/pvrtest.sh
                 This project (MIT, ncplus-MIT.txt), src/mainline/sgx/.
mainline/fire.sh, tools/*
                 This project (MIT, ncplus-MIT.txt). The tools are statically linked with the
                 glibc (LGPL-2.1, glibc-LGPL-2.1.txt) of the openlgtv buildroot-nc4 toolchain. tools/ncboot also
                 contains FreeType (portions of this software are copyright (c) The FreeType
                 Project, www.freetype.org; FreeType License), zlib (zlib License) and bzip2
                 (bzip2 License), from that toolchain; it draws with the TV's own LG font.
EOF

cat > "$P/README.txt" <<'EOF'
Debian for LG NetCast TVs (MediaTek MT5398)

A complete, modern Linux system for the TV: Debian 13 "trixie" (armhf) on mainline Linux 6.12,
with an Xfce desktop on the TV panel. It is not LG's operating system with something added: while
it runs, it replaces LG's system entirely (LG's kernel, drivers and programs are not running).

LG's system starts it without changing the TV: a kernel module (nckexec) warm-boots the new kernel
from RAM, and the Debian root is an image file on this USB stick. Nothing is written to the TV's
internal flash. Rebooting Debian, or a power cycle, brings back LG's system.

EXPERIMENTAL. Tested on one MT5398 NetCast TV. NO WARRANTY: you use it at your own risk.

Before you start
  - A root shell on the TV (telnet to port 23).
  - The TV must pass "tools/ncpreflight" (read-only). On one that doesn't, a boot that goes wrong
    can leave the TV permanently unbootable, and fire.sh refuses to boot. Don't bypass it.
    If your TV needs help passing the check, join #netcast on the openlgtv Discord.
  - For the network, a USB Ethernet adapter (ASIX AX88179/AX88772, Realtek RTL8152/8153,
    SMSC95xx, CDC ECM/NCM) or a USB Wi-Fi adapter with an Atheros AR9170 chip (carl9170 driver,
    e.g. Netgear WN111v2). The TV's built-in Wi-Fi has no Linux driver. Debian takes an address
    by DHCP; with both plugged in, Ethernet is preferred.
  - A USB keyboard and mouse. The stick and the Ethernet adapter take two of the TV's USB ports,
    so the easy choice is a wireless keyboard + mouse set with one receiver.
  - A FAT32 USB stick of 8 GB or more.

Install
  1. Unzip this archive onto the root of the stick. You get a "netcast" folder. (When updating,
     your authorized_keys and wifi.conf stay as they are, but a new debian.img replaces everything
     you installed or saved in the old one.)
  2. For SSH: in netcast/mainline, copy authorized_keys.example to authorized_keys and put your
     public key(s) in the copy, one per line. They log in as root and are copied to root's keys at
     every boot.
  3. For Wi-Fi: copy wifi.conf.example to wifi.conf there and fill in the copy (network name,
     passphrase).
     The .example files themselves are never read.

Boot
  1. Plug the stick into the TV (LG's system running; nothing needs to be on screen).
  2. Over telnet, run a dry run first. It checks everything and boots nothing:
         sh /mnt/usb/usb*/Drive1/netcast/mainline/fire.sh probe
  3. Boot (the telnet session drops):
         nohup sh /mnt/usb/usb*/Drive1/netcast/mainline/fire.sh go >/dev/null 2>&1 &
     The panel shows the Linux console, then the Xfce desktop. If fire.sh refuses, LG's system
     keeps running and netcast/mainline/fire.log says why.

Using Debian
  - Xfce logs in as user "tv" (no password; sudo without a password).
  - Ctrl+Alt+F1 is a root console, Ctrl+Alt+F7 goes back to the desktop.
  - SSH: ssh root@<the address your DHCP server gave the TV>, with the key from authorized_keys.
    root has no password login. "tv" has no keys and no password, so it can't log in over SSH until
    you set one ("sudo passwd tv"). tv can use sudo without a password, so make it a strong one.
  - apt works as on any Debian system. The image is 3 GB (FAT32 caps files at 4 GB), about
    1.6 GB of it free.
  - Network shares (SMB 2.1/3, NFS v3/v4; new in 1.0, not tried on the TV yet):
         sudo mount -t cifs //SERVER/SHARE /mnt -o username=NAME,uid=tv,gid=tv
         sudo mount -t nfs SERVER:/EXPORT /mnt
  - Text console only, no desktop: create an empty file netcast/mainline/console on the stick.
  - LG's own remote works (not other brands'): arrows, OK = Enter, Back = Esc, Vol, Mute, CH +/-
    = Page Up/Down, colours = F1-F4. Power shuts Debian down and puts the TV in standby. Three
    presses within 30 s force it off (not tested).
  - Back to LG's system: "sudo reboot" (the TV resets and starts LG's system normally, ~1 min).
    If Debian hangs, the hardware watchdog does the same within ~2.5 min.

What works
  Both CPU cores, 1920x1080 at the panel's own 50 or 60 Hz, sound through the TV speakers
  (Master volume in the desktop's mixer), USB storage, Ethernet, keyboards and mice, Bluetooth
  keyboards with a USB dongle, the LG remote, the backlight.
  Video: H.264 (up to 1080p, progressive) is decoded by the TV's hardware and shown full screen
  on the TV's video plane. Parole (Multimedia menu) plays files that way (no sound yet, see
  "Not working"); the desktop disappears while a video plays and comes back when it stops
  (keys: Space pause, Left/Right and CH +/- seek, Ctrl+Q quits Parole). Other codecs (MPEG-2,
  MPEG-4, HEVC, VP9, ...) are decoded by the CPU: fine for SD, not for HD. 1080p is scaled
  down to 720p (for now).
  50 or 60 Hz: Linux keeps the rate the panel has when it starts. fire.sh sets 60 Hz first; put
  NETCAST_HZ=50 (or NETCAST_HZ=keep: whatever LG's system is using) in front of the command to
  choose otherwise: NETCAST_HZ=50 nohup sh ... fire.sh go ...

Not working
  GPU acceleration of the desktop (Xorg draws with the CPU), the TV's built-in Wi-Fi, TV tuner
  and HDMI inputs, hardware decoding of codecs other than H.264, interlaced H.264.
  Sound: the TV speakers stayed silent on the latest test boots, even for a plain test tone
  (earlier builds played sound; cause not found yet, possibly the video plane's setup).

GPU test (experimental, for developers)
  The TV's PowerVR SGX544 runs under Debian with LG's own OpenGL ES libraries, which fire.sh
  copies from the TV into netcast/mainline/pvr/lib (they are not part of this package). The
  test renders off-screen only: nothing changes on the panel. To run it at boot, create an
  empty file netcast/mainline/pvrtest on the stick; or as root in Debian:
      sh /mnt/stick/netcast/mainline/pvr/pvrtest.sh
  Results: netcast/mainline/pvr/pvrtest.log and frame.ppm. If the GPU hangs the TV, the
  watchdog brings back LG's system.

Tools for LG's system (in tools/, run over telnet)
  ncpreflight          says whether this TV may boot another kernel (read-only). fire.sh runs it.
  ncdebug status       the debug level (read-only). "set"/"toggle" write the TV's settings EEPROM
                       (a dry run without --yes): read Safety in the project's README first.
  nckey KEY            send a remote key, e.g. "nckey home". "nckey instart" opens LG's In-Start
                       service menu; its Tool Option / Area Option / ADC / White Balance changes
                       are permanent.
  evdump [secs]        print every input event (remote, keyboard, mouse).
  ncnetkeep [-v]       Wi-Fi keep-alive for LG's system: the TV's Wi-Fi never receives broadcasts,
                       so routers lose it every ~20 min. This ARPs the gateway every 30 s.
                       Start it with: nohup tools/ncnetkeep >/dev/null 2>&1 &
  ncrate               show the panel's refresh rate; "ncrate 60" or "ncrate 50" sets it until the
                       next power cycle, "ncrate release 50" puts LG's own 50 Hz back.
  memscan              read physical memory. After a Debian boot, LG's system can read that boot's
                       kernel log back from RAM (ramoops, 1 MB at 0x3e000000):
                           tools/memscan -r 0x3e000000 0x100000 > /tmp/ramoops.bin
                       (the project's tools/ramlog.py does this over telnet and decodes it).

Source code and licences: LICENSES/NOTICE.txt.
EOF

# --- zip (plain files for FAT; debian.img is > 2 GB, so the archive is ZIP64) ---
python3 -I - "$S" "$ZIP" <<'PY'
import os, sys, zipfile
src, zp = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
    for root, dirs, files in os.walk(os.path.join(src, "netcast")):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(root, f)
            z.write(full, os.path.relpath(full, src))
PY
rm -rf "$S"
ls -la "$ZIP"
python3 -I -c 'import sys, zipfile; [print("%11d  %s" % (i.file_size, i.filename)) for i in zipfile.ZipFile(sys.argv[1]).infolist()]' "$ZIP"
