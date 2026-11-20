#!/bin/bash
# Build the NetCast+ USB package: dist/netcast/ (staging tree) and dist/netcast-plus.zip.
# Unzip onto the root of a FAT32 USB stick, plug it into the TV, then from the TV's telnet shell:
#     sh /mnt/usb/usb*/Drive1/netcast/start.sh
#   --with-tyrian-data   bundle the Tyrian 2.1 freeware data (see LICENSES/NOTICE.txt)
#   --no-dev             leave out the developer extras (chroot image + enter.sh/teardown.sh, static
#                        busybox, nckey/evdump/ncdebug/peek/sniff)
#   --release            public release build: no chroot image (it holds LG's glibc and the SDK's
#                        GPL userland), no untested kernel modules, no Tyrian data
set -euo pipefail
WITH_TYRIAN=0 WITH_DEV=1 RELEASE=0
for a in "$@"; do
  case "$a" in
    --with-tyrian-data) WITH_TYRIAN=1 ;;
    --no-dev) WITH_DEV=0 ;;
    --release) RELEASE=1 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done
if [ "$RELEASE" = 1 ] && [ "$WITH_TYRIAN" = 1 ]; then
  echo "--release: the Tyrian data carries a 1995 EULA, not bundling it" >&2; WITH_TYRIAN=0
fi
cd "$(dirname "$0")/.."
TOP=$PWD
OUT=$TOP/src/out          # binaries (make -C src, third_party/build-deps.sh)
PKG=$TOP/package/apps     # app scripts, configs and app.ini files
DIST=$TOP/dist
P=$DIST/netcast
make -C src >/dev/null
rm -rf "$DIST" && mkdir -p "$P/system" "$P/apps" "$P/LICENSES" "$P/tools"

cp "$OUT/launcher" "$P/system/launcher"
mkdir -p "$P/tools"
cp "$OUT/ncscreen" "$OUT/ncnosleep" "$OUT/nclite" "$P/tools/"
cp "$OUT/run-app.sh" "$P/apps/run-app.sh"

# --- Doom ---
mkdir -p "$P/apps/doom"
cp "$OUT/doom/chocolate-doom" "$OUT/doom/freedoom1.wad" "$P/apps/doom/"
cp "$PKG/doom/default.cfg" "$PKG/doom/chocolate-doom.cfg" "$PKG/doom/doom.sh" "$P/apps/doom/"
cat > "$P/apps/doom/app.ini" <<'EOF'
title=Doom
desc=Freedoom: Phase 1 on Chocolate Doom. BACK menu, CH- quit, OK fire, CH+ use, arrows move
exec=doom.sh
color=b02a20
order=10
EOF

# --- OpenTyrian (only if its data is bundled) ---
if [ "$WITH_TYRIAN" = 1 ]; then
  mkdir -p "$P/apps/tyrian/data"
  cp "$OUT/tyrian/opentyrian" "$PKG/tyrian/opentyrian.cfg" "$PKG/tyrian/run.sh" "$P/apps/tyrian/"
  TZ=$TOP/third_party/dl/tyrian21.zip
  [ -f "$TZ" ] || curl -sSfL -o "$TZ" https://camanis.net/tyrian/tyrian21.zip
  echo "7790d09a2a3addcd33c66ef063d5900eb81cc9c342f4807eb8356364dd1d9277  $TZ" | sha256sum -c --quiet -
  unzip -q -o -j "$TZ" 'tyrian21/*' -d "$P/apps/tyrian/data"
  cat > "$P/apps/tyrian/app.ini" <<'EOT'
title=Tyrian
desc=OpenTyrian shoot-em-up. OK fire, BACK menu, RED rear weapon, GREEN/YELLOW sidekicks
exec=run.sh
color=d08a20
order=15
EOT
  cp "$TOP/third_party/opentyrian-2.1.20260913/COPYING" "$P/LICENSES/OpenTyrian-GPL-2.0.txt"
fi

# --- Photos (HTTP photo viewer) ---
mkdir -p "$P/apps/photos"
cp "$OUT/photos" "$P/apps/photos/"
cat > "$P/apps/photos/photos.conf" <<'EOT'
# Photo server: any HTTP server with directory listings, e.g. on a PC:
#     cd ~/Pictures && python3 -m http.server 8000
# then set (use the PC's LAN address):
url=http://192.168.1.10:8000/
EOT
cat > "$P/apps/photos/run.sh" <<'EOT'
#!/bin/sh
D=$(cd "$(dirname "$0")" && pwd)
exec sh "$D/../run-app.sh" "$D/photos" "${1:-$D/photos.conf}"
EOT
cat > "$P/apps/photos/app.ini" <<'EOT'
title=Photos
desc=Photos from an HTTP server (edit apps/photos/photos.conf). OK slideshow, < > browse
exec=run.sh
color=8a3ab0
order=30
EOT

# --- RetroArch + cores (built by third_party/retro/build-cores.sh, staged in src/out/retroarch) ---
if [ -d "$OUT/retroarch" ]; then
  mkdir -p "$P/apps/retroarch"
  cp -r "$OUT/retroarch/retroarch" "$OUT/retroarch/cores" "$OUT/retroarch/info" "$OUT/retroarch/roms" "$P/apps/retroarch/"
  mkdir -p "$P/apps/retroarch/system" "$P/apps/retroarch/saves" "$P/apps/retroarch/states"
  cp -r "$PKG/retroarch/." "$P/apps/retroarch/"
  rm -f "$P/apps/retroarch/info/core_info.cache"
  cp "$OUT/retroarch/roms/240p Test Suite/LICENSE.txt" "$P/LICENSES/240p-Test-Suite-GPL-2.0.txt"
  cp "$TOP/third_party/retro/RetroArch/COPYING" "$P/LICENSES/RetroArch-GPL-3.0.txt" 2>/dev/null || true
  R=$TOP/third_party/retro
  for lic in libretro-fceumm/Copying:fceumm-GPL-2.0 snes9x2005/copyright:snes9x2005 \
             gambatte-libretro/COPYING:gambatte-GPL-2.0 Genesis-Plus-GX/LICENSE.txt:genesis_plus_gx-noncommercial \
             gpsp/COPYING:gpsp-GPL-2.0 pcsx_rearmed/COPYING:pcsx_rearmed-GPL-2.0; do
    cp "$R/${lic%%:*}" "$P/LICENSES/libretro-${lic#*:}.txt"
  done
fi

# --- Media player (hardware decoding via GStreamer 0.10 / mtkvideosink) ---
mkdir -p "$P/apps/player"
cp "$OUT/ncplay" "$P/apps/player/"
cat > "$P/apps/player/streams.conf" <<'EOT'
# One stream per line:  Name=URI
#   file:///mnt/usb/usb2/Drive1/videos/movie.mkv
#   http://192.168.1.10:8000/stream.ts      (HTTP progressive MP4/MKV/TS)
#   rtsp://192.168.1.20:554/live            (IP cameras, RTSP servers)
#   udp://239.0.0.1:1234                    (multicast MPEG-TS, e.g. from VLC/ffmpeg)
# Hardware decoding: H.264, MPEG-2, MPEG-4 video; AAC (ADTS/TS), MP3, AC-3 audio.
EOT
cat > "$P/apps/player/run.sh" <<'EOT'
#!/bin/sh
D=$(cd "$(dirname "$0")" && pwd)
exec sh "$D/../run-app.sh" "$D/ncplay" "${1:-$D/streams.conf}"
EOT
cat > "$P/apps/player/app.ini" <<'EOT'
title=Media Player
desc=Hardware-decoded video: files, HTTP, RTSP, UDP multicast (edit apps/player/streams.conf)
exec=run.sh
color=c0306a
order=25
EOT

# --- Settings (LVGL) ---
mkdir -p "$P/apps/settings"
cp "$OUT/ncsettings" "$PKG/settings/run.sh" "$PKG/settings/app.ini" "$P/apps/settings/"
cp "$TOP/third_party/lvgl/LICENCE.txt" "$P/LICENSES/LVGL-MIT.txt"

# --- shared UI fonts (DejaVu) ---
mkdir -p "$P/system/fonts"
cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf "$P/system/fonts/"

# --- Music (FLAC / MP3 / WAV) ---
mkdir -p "$P/apps/music"
cp "$OUT/ncmusic" "$P/apps/music/"
cat > "$P/apps/music/run.sh" <<'EOT'
#!/bin/sh
# Music: no arguments = all music on the USB stick; otherwise files, folders, .m3u or http:// URLs
D=$(cd "$(dirname "$0")" && pwd)
exec sh "$D/../run-app.sh" "$D/ncmusic" "$@"
EOT
cat > "$P/apps/music/app.ini" <<'EOT'
title=Music
desc=FLAC, MP3 and WAV from the USB stick or network shares, with cover art
exec=run.sh
color=d81b60
order=27
EOT
printf 'dr_flac / dr_mp3 / dr_wav (mackron/dr_libs @ %s): public domain (Unlicense) or MIT No Attribution.\n' "$(cat "$TOP/third_party/dr_libs/COMMIT")" > "$P/LICENSES/dr_libs.txt"

# --- Network Shares (SMB2/3 via libsmb2) + gateway for the other apps ---
mkdir -p "$P/apps/smb"
cp "$OUT/ncsmb" "$OUT/smbgw" "$P/apps/smb/"
[ -f "$P/apps/smb/shares.conf" ] || cat > "$P/apps/smb/shares.conf" <<'EOT'
# Network Shares: one server or share per line.
#   Name = smb://[DOMAIN;]user[:password]@host[:port]/[share[/folder]]
# Leave out the share to list all shares of a server. No user = guest.
# Escape special characters in the password: @ -> %40, : -> %3A, space -> %20, % -> %25
# SMB2/3 servers only (Windows 7+, Samba 3.6+, any current NAS).
#
# Examples:
#   NAS music  = smb://me:secret@192.168.1.20/music
#   Windows PC = smb://me:secret@192.168.1.30/
EOT
cat > "$P/apps/smb/run.sh" <<'EOT'
#!/bin/sh
D=$(cd "$(dirname "$0")" && pwd)
exec sh "$D/../run-app.sh" "$D/ncsmb" "$@"
EOT
cat > "$P/apps/smb/app.ini" <<'EOT'
title=Network Shares
desc=Browse SMB shares and the USB stick; play music and video, view photos
exec=run.sh
color=00897b
order=22
EOT
cp "$TOP/third_party/libsmb2-libsmb2-6.3/LICENCE-LGPL-2.1.txt" "$P/LICENSES/libsmb2-LGPL-2.1.txt"

# --- Terminal (libvterm) ---
mkdir -p "$P/apps/terminal"
cp "$OUT/ncterm" "$PKG/terminal/run.sh" "$PKG/terminal/app.ini" /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf "$P/apps/terminal/"
cp /usr/share/doc/fonts-dejavu-core/copyright "$P/LICENSES/DejaVu-fonts.txt"
cp "$TOP/third_party/libvterm-0.3.3/LICENSE" "$P/LICENSES/libvterm-MIT.txt"

# --- Remote Desktop (FreeRDP 3, SDL2 client) ---
mkdir -p "$P/apps/rdp"
cp "$OUT/rdp/sdl-freerdp" "$PKG/rdp/run.sh" "$PKG/rdp/app.ini" "$PKG/rdp/rdp.conf" "$P/apps/rdp/"
cp "$TOP/third_party/FreeRDP-3.33.1/LICENSE" "$P/LICENSES/FreeRDP-Apache-2.0.txt"
cp "$TOP/third_party/SDL2_ttf-2.24.0/LICENSE.txt" "$P/LICENSES/SDL2_ttf-zlib.txt"

# --- QML Hello (static Qt 6.12 + eglfs_netcast), once built ---
if [ -x "$OUT/qmlhello/qmlhello" ]; then
  mkdir -p "$P/apps/qmlhello/fonts"
  cp "$OUT/qmlhello/qmlhello" "$PKG/qmlhello/run.sh" "$PKG/qmlhello/app.ini" "$P/apps/qmlhello/"
  cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf "$P/apps/qmlhello/fonts/"
  cp "$TOP/third_party/qt/src/qtbase-everywhere-src-6.12.0/LICENSES/LGPL-3.0-only.txt" "$P/LICENSES/Qt-LGPL-3.0.txt"
  cat > "$P/LICENSES/Qt-NOTE.txt" <<'NOTE'
QML Hello is statically linked with Qt 6.12.0 (LGPL-3.0). The complete source of the app, the
eglfs_netcast plugin and the Qt build scripts (third_party/qt/build-*.sh) are in the NetCast+
source tree, so the app can be rebuilt and relinked against a modified Qt.
NOTE
fi

# --- Kernel modules for the stock 3.0.13 kernel (never loaded automatically) ---
if [ "$RELEASE" = 0 ] && [ -f "$TOP/src/kmod/xpad.ko" ]; then   # third_party/build-deps.sh kmod
  mkdir -p "$P/tools/modules"
  cp "$TOP/src/kmod/nchello.ko" "$TOP/src/kmod/xpad.ko" "$P/tools/modules/"
fi

# --- Developer extras: static busybox, chroot userland (enter.sh / teardown.sh), debug tools ---
if [ "$WITH_DEV" = 1 ]; then
  cp "$OUT/nckey" "$OUT/evdump" "$OUT/ncdebug" "$OUT/peek" "$OUT/sniff" "$P/tools/"
  U=$TOP/usb/netcast   # build/build-image.sh output
  BBX=$U/busybox; [ -f "$BBX" ] || BBX=$TOP/build/busybox-1.36.1/busybox
  if [ -f "$BBX" ]; then
    cp "$BBX" "$P/busybox"
    cp "$TOP/build/busybox-1.36.1/LICENSE" "$P/LICENSES/BusyBox-GPL-2.0.txt" 2>/dev/null \
      || echo "warning: BusyBox LICENSE not found (build/busybox-1.36.1/)" >&2
    cp "$U/enter.sh" "$U/teardown.sh" "$P/"
    if [ "$RELEASE" = 1 ]; then
      echo "note: --release: no chroot image; build your own with build/build-image.sh" >&2
    elif [ -f "$U/netcast-root.ext4.gz" ]; then
      gunzip -c "$U/netcast-root.ext4.gz" > "$P/netcast-root.ext4"
    else
      echo "note: no chroot image (run tools/pull-glibc.sh && build/build-image.sh); Terminal uses the TV shell" >&2
    fi
  else
    echo "note: no static busybox (build/build-image.sh builds it); chroot scripts left out" >&2
  fi
fi

# --- SDL demo ---
mkdir -p "$P/apps/sdldemo"
cp "$OUT/sdldemo" "$P/apps/sdldemo/"
cat > "$P/apps/sdldemo/run.sh" <<'EOF'
#!/bin/sh
D=$(cd "$(dirname "$0")" && pwd)
exec sh "$D/../run-app.sh" "$D/sdldemo" 3600
EOF
cat > "$P/apps/sdldemo/app.ini" <<'EOF'
title=SDL Demo
desc=GPU graphics + sound test. Arrows move, OK bursts, BACK exits
exec=run.sh
color=2a6ad0
order=20
EOF

# --- input console ---
mkdir -p "$P/apps/inputtest"
cp "$OUT/nctext" "$P/apps/inputtest/"
cat > "$P/apps/inputtest/run.sh" <<'EOF'
#!/bin/sh
D=$(cd "$(dirname "$0")" && pwd)
exec sh "$D/../run-app.sh" "$D/nctext" 600
EOF
cat > "$P/apps/inputtest/app.ini" <<'EOF'
title=Input Test
desc=Shows every remote / keyboard / mouse event. BACK exits
exec=run.sh
color=3a8a50
order=90
EOF

# --- entry point ---
cat > "$P/start.sh" <<'EOF'
#!/bin/sh
# NetCast+ entry point. From the TV's telnet shell:
#     sh /mnt/usb/usb2/Drive1/netcast/start.sh          (detaches; log in /tmp/netcast.log)
#     sh .../start.sh --fg                               (stay in the foreground)
# Nothing is written to the TV's internal flash: apps keep their files in this folder.
N=$(cd "$(dirname "$0")" && pwd)
case "$N" in /mnt/usb/*) ;; *) echo "start.sh: run me from the USB stick" >&2; exit 1;; esac
if [ "$1" != "--fg" ]; then
  if pidof launcher >/dev/null 2>&1; then echo "NetCast+ is already running"; exit 0; fi
  nohup sh "$N/start.sh" --fg > /tmp/netcast.log 2>&1 &
  echo "NetCast+ started (log: /tmp/netcast.log)"
  exit 0
fi
# Settings (written by the Settings app): SAVER_MIN, NOSLEEP, LITE
SAVER_MIN=20; NOSLEEP=1; LITE=0
[ -f "$N/home/settings.conf" ] && . "$N/home/settings.conf"
# Keep the TV on (RAM-only: LG's Auto Off / No-signal / No-operation timers off until power cycle)
[ "$NOSLEEP" = 1 ] && "$N/tools/ncnosleep" off
# Our own screen saver: backlight off after N min without any input, any key brings it back
if [ "${SAVER_MIN:-0}" -gt 0 ] && ! pidof ncscreen >/dev/null 2>&1; then
  nohup "$N/tools/ncscreen" saver "$SAVER_MIN" > /tmp/ncscreen-saver.log 2>&1 &
fi
# Lite mode: stop optional LG services
[ "$LITE" = 1 ] && sh "$N/apps/run-app.sh" "$N/tools/nclite" stop > /tmp/nclite.log 2>&1
# Apps can queue a follow-up (e.g. Network Shares -> Music -> back) in /tmp/ncmenu.next
export NETCAST_SESSION=1
while :; do
  rm -f /tmp/ncmenu.choice /tmp/ncmenu.next
  sh "$N/apps/run-app.sh" "$N/system/launcher" --service lg.service.normal.netcastplus "$N/apps"
  C=$(cat /tmp/ncmenu.choice 2>/dev/null)
  if [ -z "$C" ] || [ "$C" = exit ]; then break; fi
  E=$(grep '^exec=' "$C/app.ini" | head -1 | cut -d= -f2-)
  if [ -n "$E" ]; then echo "== launching $C/$E"; (cd "$C" && sh "./$E"); fi
  while [ -f /tmp/ncmenu.next ]; do
    mv /tmp/ncmenu.next /tmp/ncmenu.run && echo "== next: $(head -1 /tmp/ncmenu.run)" && sh /tmp/ncmenu.run
  done
done
echo "NetCast+ exited"
EOF

cat > "$P/README.txt" <<'EOF'
NetCast+ : native apps for LG NetCast 4.x TVs (MT5398 / PowerVR SGX, 2013-2014 models)

Install
  1. Unzip this archive onto the root of a FAT32 USB stick (you get a "netcast" folder).
  2. Plug the stick into the TV.
  3. Get a root shell on the TV (telnet) and run:
         sh /mnt/usb/usb*/Drive1/netcast/start.sh
     (the USB path may be usb1/usb2 depending on the port)

Use
  Arrows move, OK launches. "Exit to TV" returns to LG's home screen.
  Inside apps, the remote's BACK key is Escape ("back / menu"). Volume and Mute work in every app.
  Photos: set url= in apps/photos/photos.conf to a server with directory listings
        (e.g. "python3 -m http.server 8000" in your Pictures folder on a PC).
  Network Shares: add servers to apps/smb/shares.conf (smb://user:password@host/share).
        OK on music opens Music, on photos opens Photos, on a video opens the Player;
        you come back to the same folder afterwards. RED plays all music in a folder.
  Music: FLAC/MP3/WAV from the stick or shares. OK play/pause, LEFT/RIGHT seek, CH+ next,
        RED previous, GREEN screen off (keeps playing), YELLOW shuffle, BLUE repeat.
  Doom: BACK menu, CH- quit (OK to confirm), OK fire / select, CH+ use, arrows move, RED/GREEN strafe,
        YELLOW run, BLUE automap, 1-7 weapons. A USB keyboard also works.

Adding apps
  Create apps/<name>/ with an app.ini (title=, desc=, exec=, color=, order=)
  and a start script. Native apps link libnc or the bundled SDL2 "netcast" driver
  and are started through apps/run-app.sh, which sets up LG's app environment.

Tools (in tools/, started automatically by start.sh where noted)
  ncnosleep status|off    LG's Auto Off / No-signal / No-operation timers (start.sh: off, RAM only)
  ncscreen on|off         backlight via LG's MICOM; "off --wake" until a key is pressed
  ncscreen saver [min]    screen saver daemon (start.sh: 20 min, set NETCAST_SAVER_MIN)
  nclite stop|start|list  stop/restart optional LG services (browsers, voice, Skype...) for RAM

Developer extras (left out with build/package.sh --no-dev)
  sh enter.sh             chroot into the userland image netcast-root.ext4 (Python, GCC, ...) if it is in
                          this folder (release zips don't ship it: build it with build/build-image.sh);
                          mounted at /tmp/ncroot, writes go only into the image file on this stick.
                          The Terminal app opens it automatically.
  sh teardown.sh          unmount the chroot again (exit its shells first)
  busybox                 static BusyBox 1.36 (md5sum, losetup, ... that LG's busybox lacks)
  tools/nckey KEY         inject a remote key into /dev/input/event2, e.g. "nckey instart" opens
                          LG's In-Start service menu (careful: Tool Option / Area Option / ADC / White
                          Balance changes are permanent; IN-STOP 897/898 is refused)
  tools/evdump [secs]     print every input event (remote, keyboard, mouse)
  tools/ncdebug status    LG debug level (NVM EEPROM). "set"/"toggle" write the EEPROM: read
                          FINDINGS.md (U-Boot boot flow) first
  tools/peek, sniff       ptrace helpers used for reverse engineering LG's TV process

Safety
  Nothing is written to the TV's internal flash: HOME, configs and saves live in
  this folder, GStreamer's registry is redirected to RAM. Power-cycle the TV to
  return to stock.
EOF

# --- strip debug info from our ARM binaries (keeps the stick small) ---
STRIP="$TOP/toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot/bin/arm-lgtv-linux-gnueabi-strip"
for f in "$P/system/launcher" "$P/tools/ncscreen" "$P/tools/ncnosleep" "$P/tools/nclite" \
         "$P/apps/music/ncmusic" "$P/apps/smb/ncsmb" "$P/apps/smb/smbgw" "$P/apps/settings/ncsettings" \
         "$P/apps/terminal/ncterm" "$P/apps/photos/photos" "$P/apps/player/ncplay"; do
  [ -f "$f" ] && "$STRIP" "$f"
done

# --- licences ---
cp "$OUT/doom/FREEDOOM-COPYING.txt" "$P/LICENSES/Freedoom-BSD.txt"
cp "$TOP/third_party/chocolate-doom-chocolate-doom-3.1.1/COPYING.md" "$P/LICENSES/ChocolateDoom-GPL-2.0.md"
cp "$TOP/third_party/SDL2-2.32.10/LICENSE.txt" "$P/LICENSES/SDL2-zlib.txt"
cp "$TOP/third_party/SDL2_mixer-2.8.2/LICENSE.txt" "$P/LICENSES/SDL2_mixer-zlib.txt"
cp "$TOP/LICENSE" "$P/LICENSES/NetCast+-MIT.txt"
cat > "$P/LICENSES/NOTICE.txt" <<'EOF'
NetCast+ itself: MIT (NetCast+-MIT.txt); the kernel modules in tools/modules are GPL-2.0.

Source code: NetCast+ and the exact build recipe for every bundled program are at
https://github.com/ddagunts/ncplus (third_party/build-deps.sh pins each upstream release or git
commit, with checksums). The upstream sources are available from the projects listed below.

Bundled components:
  Chocolate Doom 3.1.1 (GPL-2.0) - https://github.com/chocolate-doom/chocolate-doom
  Freedoom 0.13.0 (BSD-3-Clause)  - https://freedoom.github.io
  SDL2 2.32.10 + NetCast video/audio driver (zlib) - https://libsdl.org
  SDL2_mixer 2.8.2 (zlib)
  FreeType (FreeType License), statically linked into the launcher/input test/photos
  stb_image (public domain / MIT) - https://github.com/nothings/stb
  OpenTyrian (GPL-2.0) - https://github.com/opentyrian/opentyrian   [only with --with-tyrian-data]
  RetroArch 1.22.2 (GPL-3.0) + libretro cores - https://github.com/libretro
    fceumm, gambatte, gpsp, pcsx_rearmed (GPL-2.0); snes9x2005 (see libretro-snes9x2005.txt);
    genesis_plus_gx (non-commercial license: may not be sold or used commercially)
  BusyBox 1.36.1 (GPL-2.0, static) - https://busybox.net
  240p Test Suite (GPL-2.0) - https://github.com/pinobatch/240p-test-mini
  libsmb2 6.3 (LGPL-2.1, statically linked into Network Shares/smbgw; full source and build scripts are in
    the NetCast+ tree, so they can be relinked against a modified libsmb2) - https://github.com/sahlberg/libsmb2
  dr_flac / dr_mp3 / dr_wav (public domain or MIT-0) - https://github.com/mackron/dr_libs
  SDL2_ttf 2.24.0 (zlib), DejaVu fonts (Bitstream Vera / public domain), LVGL 9.6 (MIT), libvterm 0.3.3 (MIT),
    FreeRDP 3.33.1 (Apache-2.0)
  Tyrian 2.1 data: freeware release by its author, fetched from https://camanis.net/tyrian/tyrian21.zip
    (as OpenTyrian's get_data.sh does). The zip still carries the original 1995 EULA; check before
    redistributing a package that contains it.
EOF

# --- zip (FAT-friendly: plain files, no symlinks) ---
python3 -I - "$DIST" <<'PY'
import os, sys, zipfile
dist = sys.argv[1]
zp = os.path.join(dist, "netcast-plus.zip")
with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk(os.path.join(dist, "netcast")):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(root, f)
            z.write(full, os.path.relpath(full, dist))
print(zp, os.path.getsize(zp))
PY
(cd "$DIST" && find netcast -type f | sort)
