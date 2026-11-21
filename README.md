# NetCast+

**A new userland and a modern native UI for 2013–2014 LG NetCast smart TVs: on the stock
2017 firmware and Linux 3.0 kernel, running from a USB stick, without modifying the TV.**

![Doom on an LG NetCast TV: from LG's home screen to Freedoom](docs/media/doom-on-lg-netcast.gif)

[![Watch the video](docs/media/doom-on-lg-netcast.jpg)](docs/media/doom-on-lg-netcast.mp4)

▶ [Video (H.264, 720p)](docs/media/doom-on-lg-netcast.mp4)

---

## What this is

LG's NetCast TVs (LB-series and similar, MediaTek MT5398) were abandoned years ago: a
web-app SDK from 2014, an old WebKit browser, and Linux 3.0.13 with glibc 2.12. The hardware
is still capable: a dual Cortex-A9, a PowerVR SGX544 GPU, 726 MB of RAM and hardware video
decoders.

NetCast+ runs on that hardware next to LG's own software and takes over the parts that matter:

- **A modern native userland:** GCC 16, Python 3.14, SDL2, FreeType and GStreamer 1.24. They
  are cross-built against the TV's own glibc and run from the USB stick, either directly or in
  a chroot.
- **Native, GPU-accelerated apps:** our own runtime gets a window from LG's process manager
  and renders with OpenGL ES 2 on the PowerVR GPU at about 57 fps.
- **An SDL2 port** with `netcast` video and audio drivers, so ordinary SDL2 programs
  (Doom, Tyrian, ...) build and run unchanged.
- **A new home screen** ("NetCast+" launcher) with drop-in apps.
- **Control of the TV itself:** backlight on/off through LG's power microcontroller, keeping the
  TV awake, a screen saver, and stopping LG services that aren't needed.

LG's own TV stack keeps running underneath (tuner, HDMI, picture processing, Wi-Fi), and you
can return to it at any time. Everything lives on the USB stick: **nothing is written to the
TV's internal flash**, and a power cycle restores the stock TV.

> **No warranty, no responsibility for bricked TVs.** These TVs are easy to brick: a bad flash write, a
> wrong service-menu setting or a kernel experiment can leave one permanently dead (secure boot burns an
> eFuse on a signature failure). Everything here is provided as is, and you use it at your own risk.
> Read [Safety](#safety) before you start.

## What it does with the TV

| Area | What NetCast+ does | How |
|---|---|---|
| **Display** | Full-screen 1280x720 GLES2 window above LG's UI, opaque or deliberately see-through over live TV | LG's HOA window API + PowerVR EGL; framebuffer alpha controlled per frame |
| **Input** | Remote, USB keyboards/mice (hot-plug), gamepad | Linux evdev; remote buttons mapped to standard keys (Back = Escape, colour keys = F1–F4); volume keys drive LG's volume |
| **Audio** | Low-latency PCM from SDL apps | MediaTek `mtkaudiosink` (GStreamer 0.10) via a new SDL audio driver |
| **Screen power** | Backlight off/on while the TV, Wi-Fi and apps keep running | LG's MICOM power controller over I2C, using the same command LG's Screen Off uses (reverse-engineered) |
| **Screen saver** | Backlight off after N minutes without input; any key wakes it | `ncscreen saver` daemon |
| **Stay awake** | Disables LG's Auto Off / No-signal / No-operation power-off timers | Patches LG's in-RAM settings copy (RAM only, never saved) |
| **Resources** | Stops unneeded LG services (browsers, voice, Skype, DIAL...) | LG's own process-manager API |
| **Wi-Fi** | Keeps the TV reachable (stock firmware drops off the network for ~5 min every ~20 min) | `ncnetkeep` ARP keep-alive: the Wi-Fi firmware never delivers broadcasts, so the router's ARP entry ages out |
| **Video** | Hardware H.264/MPEG decode on the video plane, our UI composited on top | GStreamer 0.10 `playbin2` + MediaTek `mtkvideosink` |
| **Userland** | Modern toolchain, Python, a shell environment | Buildroot (openlgtv buildroot-nc4) on LG's glibc; optional chroot image |
| **Kernel** | Extra modules for the stock kernel (gamepads first) | Vanilla 3.0.13 built to the TV's exact vermagic (insmod pending a layout check) |

### Apps

| App | |
|---|---|
| **Launcher** | NetCast+ home screen; apps are folders with an `app.ini` |
| **Doom** | Chocolate Doom + Freedoom: Phase 1, with sound and OPL music, playable on the remote |
| **Tyrian** | OpenTyrian (optional freeware data) |
| **Photos** | Browses an HTTP server (e.g. `python3 -m http.server`): thumbnail grid, prefetch, EXIF rotation, slideshow |
| **Network Shares** | SMB2/3 browser (libsmb2) for NAS/Windows shares and the USB stick; opens music, photos and video in the other apps |
| **Music** | FLAC (incl. 24-bit/96 kHz), MP3 and WAV with tags, cover art, playlist, seek, shuffle/repeat; plays from the stick or SMB |
| **Player** | Hardware-decoded video (H.264/MPEG-2/MPEG-4; MP4, MKV, TS, FLV) from files, HTTP, RTSP or UDP streams, using the TV's own decoders |
| **RetroArch** | RetroArch 1.22 (GLES2) with NES, SNES, Game Boy, GBA, Mega Drive and PlayStation cores |
| **Settings** | LVGL 9 settings screen: screen saver, keep-awake, lite mode, Wi-Fi keep-alive, screen off, system info |
| **Terminal** | libvterm terminal (USB keyboard) for the TV shell or the chroot userland |
| **Remote Desktop** | FreeRDP 3 (SDL2 client) for Windows or xrdp desktops |
| **QML Hello** | Qt 6 / QML on the TV through an `eglfs_netcast` platform plugin (in progress) |
| **SDL Demo / Input Test** | GPU, audio and input showcases |

## How it works

```
                 ┌──────────────────────── USB stick (FAT32) ────────────────────────┐
                 │  start.sh ─ launcher ─ apps (Doom, Tyrian, Photos, ...)            │
                 │                 │                                                  │
                 │        SDL2 (netcast video + audio)       tools: ncscreen,         │
                 │                 │                          ncnosleep, nclite       │
                 │               libnc  (register app, window, EGL, input, logging)   │
                 └─────────────────┼──────────────────────────────────────────────────┘
   LG userspace (stock, untouched) │
     procmgr ── window ID ─────────┤  HOA API (libafopenapi) over D-Bus
     RELEASE (TV process) ─────────┤  settings in RAM (ptrace), MICOM I2C protocol
     PowerVR EGL/GLES2 ────────────┤  native window = HOA window ID
     GStreamer 0.10 mtkaudiosink ──┘
   Kernel 3.0.13 (stock) ── /dev/mtal (MediaTek), /dev/pvrsrvkm (GPU), /dev/input/event*
```

The key steps, all reverse-engineered from LG's binaries (no LG headers or SDK were involved):

1. **Become an LG app.** Copy the environment `procmgr` gives its apps, register with
   `HOA_PROC_RegisterService` (it reads `PM_EXECARGS`, not argv), and get the window
   `procmgr` assigns. The **EGL native window is simply that window ID.**
2. **Tame LG's libraries.** Initialise LG's debug framework first and keep its crash and
   stdout hooks out of our processes. Redirect every path LG would write to flash
   (`HOME`, the GStreamer registry, the app-framework mask) into RAM or onto the USB stick.
3. **Talk to the hardware the way LG does.** PCM goes through MediaTek's GStreamer sink.
   Backlight commands go over the MICOM's I2C bus, using the exact bytes LG's TV process
   sends, captured live with ptrace.

All the details, gotchas and addresses are in **[FINDINGS.md](FINDINGS.md)**.

## Getting started

Requirements: an LG NetCast 4.x TV with a root shell (telnet), and a FAT32 USB stick.
Getting that root shell is model- and firmware-specific and is not covered here; see the
[openlgtv](https://github.com/openlgtv) community.

1. Download `netcast-plus-<version>.zip` from [Releases](https://github.com/ddagunts/ncplus/releases)
   (or build it, see [Building](#building)) and unzip it onto the root of the stick.
   You get a `netcast/` folder.
2. Plug the stick into the TV.
3. From the TV's shell:
   ```sh
   sh /mnt/usb/usb*/Drive1/netcast/start.sh      # NetCast+ launcher (detaches)
   ```
   (The stick shows up as `usb1` or `usb2` depending on the port.)

`start.sh` switches LG's sleep timers off (in RAM), starts the screen saver and the Wi-Fi
keep-alive, and shows the launcher. Arrows move, OK launches, and **Back is back/menu**, and volume/mute work inside apps.
"Exit to TV" returns to LG's home screen.

Tools on the stick (`netcast/tools/`):

```sh
ncscreen off --wake        # backlight off until any key is pressed
ncscreen saver 20          # screen saver: dark after 20 min idle
ncnosleep status|off       # LG power-off timers (RAM only)
nclite stop|start|list     # stop / restart optional LG services
ncnetkeep [-t 30] [-v]     # Wi-Fi keep-alive (start.sh runs it; NETKEEP=0 in netcast.conf turns it off)
```

Developer tools (`make -C src out/<tool>`; not in the package):

```sh
nckey instart              # inject a remote key, e.g. open LG's In-Start service menu (code 899)
evdump 30                  # print every input event for 30 s
ncdebug status             # read LG's debug level (NVM EEPROM); set/toggle write the EEPROM, see FINDINGS.md
```

> **Careful in LG's service menu.** Changes under Tool Option, Area Option, ADC or White Balance are
> saved permanently. `nckey` refuses codes 897/898 (IN-STOP / power-only), which reset the TV to its
> shipping state.

## Building

Everything cross-compiles on an x86-64 Linux host (tested on Debian 13). You need `curl`, `git`,
`unzip`, `python3`, `patch`, a host `gcc`/`make`, and `fonts-dejavu-core` (the UI fonts are copied from
`/usr/share/fonts`). No files from LG are needed: EGL/GLES2 are linked against stubs generated from
the TV libraries' export lists (`src/stubs/`).

```sh
# 1. Toolchain: the openlgtv buildroot-nc4 SDK (release 2026.08-webos, "arm-lgtv-linux-gnueabi"),
#    unpacked and relocated to toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot/
#    https://github.com/openlgtv/buildroot-nc4/releases
# 2. Third-party libraries and apps: fetched at pinned versions, checksummed, cross-built
third_party/build-deps.sh             # SDL2 (+ netcast patch), SDL2_mixer/ttf, libsmb2, libvterm, LVGL,
                                      # Chocolate Doom + Freedoom, OpenTyrian, FreeRDP, RetroArch + cores
# 3. NetCast+ itself
make -C src                           # libnc, launcher, apps, tools
# 4. The USB package
build/package.sh                      # -> dist/netcast-plus.zip
build/package.sh --with-tyrian-data   #    ... with the Tyrian 2.1 freeware data (see its EULA)
build/package.sh --release            #    ... as published: no chroot image or Tyrian data; xpadone.ko only
```

Optional:

```sh
third_party/build-deps.sh kmod        # vanilla 3.0.13 with the TV's vermagic + src/kmod modules
third_party/qt/fetch.sh && third_party/qt/build-host.sh && third_party/qt/build-target.sh   # Qt 6 (WIP)
tools/pull-glibc.sh && build/build-image.sh   # chroot image with the full userland (needs LG's glibc
                                              # from your TV) -> usb/netcast/
```

Host tools talk to the TV over telnet. Put its IP address in `tools/.tv-host` (or set `NETCAST_TV`).
`tools/tv.py 'cmd'` runs a command, `tools/push.sh <subdir> files...` uploads into the stick's
`netcast/` folder through a temporary BusyBox `ftpd` confined to the stick, and `tools/pull.sh` copies a
file back. `make -C src deploy` uploads all apps.

### Repository layout

| Path | |
|---|---|
| `src/libnc/` | the native runtime: HOA registration, window, EGL, input, text (FreeType), 2D helpers |
| `src/apps/` | launcher, photos, player, music, network shares, settings, terminal, demos, `ncscreen`, `nclite`, `run-app.sh` |
| `src/tools/` | `ncnosleep`, `ncnetkeep`, `nckey`, `evdump`, `ncdebug`, `wlq`/`pktlog` (Wi-Fi diagnostics), plus `peek`/`sniff` (ptrace helpers used for reverse engineering) |
| `src/stubs/` | export lists of the TV's `libEGL`/`libGLESv2`, used to generate link stubs |
| `src/kmod/` | kernel modules for the stock 3.0.13 kernel (`xpadone`, `nckexec` warm boot) |
| `src/qml/` | QML apps (static Qt 6 + `eglfs_netcast`) |
| `package/apps/` | per-app launch scripts, configs and `app.ini` files copied into the package |
| `third_party/` | `build-deps.sh` (pinned versions + checksums), the SDL2 and qtbase patches, RetroArch/Qt build scripts, vendored stb, dr_libs and Khronos headers |
| `build/` | package builder, chroot image builder (+ BusyBox config) |
| `usb/netcast/` | chroot helpers (`enter.sh`, `teardown.sh`) for the optional userland image |
| `tools/` | host-side helpers (`tv.py`, `push.sh`, `pull.sh`, `pull-glibc.sh`, `netwatch.sh`, `kmod-check.sh`, `tv-dump-flash.sh`) |
| `FINDINGS.md` | the full technical notes |

## Safety

The TV's flash is treated as untouchable, because modifying it is believed to brick these sets:

- All code, data, configs and saves live on the USB stick; working state lives in `/tmp` (RAM).
- LG paths that would write flash (`HOME=/home`, the GStreamer registry, the app-framework mask)
  are redirected for every app.
- Changes to LG's behaviour (sleep timers, services, backlight) are **RAM-only** and reset on a
  power cycle. NetCast+ never calls LG's settings-save routine.
- Firmware-specific tools verify LG's code before touching anything, and refuse on other builds.
- **Never boot a different kernel unless the TV's debug level is DEBUG.** `nckexec.ko` can warm-boot
  an unsigned kernel from RAM. On a TV in RELEASE (most retail sets; check with `ncdebug status`), a kernel
  that misbehaves can end in U-Boot's signature-failure path at the next boot: a hang plus an eFuse burn,
  with no serial console to recover. That bricks the TV permanently. DEBUG skips the eFuse burn and allows
  UART break-in. `nckexec-fire.sh` refuses to run unless `ncdebug` reports DEBUG. Changing the level is a
  write to the NVM EEPROM: read FINDINGS.md ("Debug level") first.
- Known exception: one 1.9 KB crash log from an early development crash, written by LG's crash
  handler before our handlers took over. It was left in place, because deleting it would be a
  flash write too.

## Status

Working on an LG NetCast 4.5 TV (software v04.03.10, May 2017): native GLES2 apps, SDL2 video and
audio, Doom, Tyrian, Photos, RetroArch, the hardware video player, launcher, backlight control,
screen saver and keep-awake.

Wi-Fi: the TV no longer drops off the network every ~20 minutes (`ncnetkeep`, see FINDINGS.md
"Wi-Fi drops").

Kernel modules load on the stock kernel: `xpadone.ko` adds Xbox One/Series wired pads (Xbox 360 and PS3
pads work out of the box). Gamepads navigate the launcher, Photos and the Player, and work in Doom.

Built, waiting for a test on the TV: Settings (LVGL), Terminal, Remote Desktop (FreeRDP). Qt 6 / QML is
being cross-built.

Next:
- More kernel modules: USB serial, tun/VPN (check struct offsets against LG's kernel, see FINDINGS.md).
- Mute the TV while the screen saver is dark.
- Magic Remote pointer support.
- More apps: ScummVM, a file browser, a lightweight browser.
- A newer kernel (e.g. Sony's MT53xx 3.10 tree) is only a research item: LG's display, GPU,
  audio and Wi-Fi drivers are binary modules tied to 3.0.13. `nckexec.ko` can warm-boot a kernel from
  RAM (tested with the stock kernel), but only try other kernels on a TV set to DEBUG (see [Safety](#safety)).

## Credits

[Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom) (GPL-2.0) ·
[Freedoom](https://freedoom.github.io) (BSD) ·
[OpenTyrian](https://github.com/opentyrian/opentyrian) (GPL-2.0; Tyrian 2.1 data is a freeware release by its author) ·
[RetroArch](https://www.retroarch.com) and libretro cores (GPL) · [240p Test Suite](https://github.com/ArtemioUrbina/240pTestSuite) (GPL-2.0) ·
[libsmb2](https://github.com/sahlberg/libsmb2) (LGPL-2.1) · [dr_libs](https://github.com/mackron/dr_libs) (public domain / MIT-0) ·
[LVGL](https://lvgl.io) (MIT) · [libvterm](https://www.leonerd.org.uk/code/libvterm/) (MIT) ·
[FreeRDP](https://www.freerdp.com) (Apache-2.0) · [Qt](https://www.qt.io) (LGPL-3.0) · DejaVu fonts ·
[SDL](https://libsdl.org) and SDL2_ttf (zlib) · [stb](https://github.com/nothings/stb) (public domain) · FreeType ·
[openlgtv / buildroot-nc4](https://github.com/openlgtv/buildroot-nc4) and the openlgtv community.

## License

NetCast+'s own code is under the [MIT License](LICENSE). Exceptions: the kernel modules in
`src/kmod/` are GPL-2.0, and the patches in `third_party/patches/` follow the license of the project
they modify (zlib for SDL, LGPL-3.0/GPL for Qt). The vendored stb, dr_libs and Khronos headers keep
their own licenses. Bundled third-party programs keep theirs; the USB package lists them in
`netcast/LICENSES/`.

Not affiliated with LG Electronics. Provided as is, without warranty of any kind. The authors take no
responsibility for bricked or damaged TVs: these TVs are easy to brick. Use at your own risk.
