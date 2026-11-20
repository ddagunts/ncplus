# LG NetCast TV hacking — findings

Last updated: 2026-11-20. `telnet <TV IP>` gives a root BusyBox shell (no login); the host tools read the IP from `$NETCAST_TV` or `tools/.tv-host`.

**Status:** a native C program built here draws GPU-accelerated graphics on the TV screen
(EGL + OpenGL ES 2, 1280x720 at 57 fps), above LG's UI. Confirmed visually.

## Ground rules

- **Never write to the TV's internal eMMC** (believed to brick the TV). **Always upload to the TV through the temporary busybox ftpd** (`tools/push.sh`). Only the USB stick
  (`/mnt/usb/usb2/Drive1`, vfat, exec allowed) and tmpfs `/tmp` (354 MB RAM) are used.
- Indirect flash writers to avoid:
  - `gst-launch` (`GST_REGISTRY=/mnt/flash/data/gst_registry.arm.bin`)
  - `gdb` (`GDB_ROOT=/mnt/lg/cmn_data`)
  - LG crash framework dumps (`/mnt/lg/cmn_data/*_pid*.txt`, `exc_log_*`)
  - app store installs (`/mnt/appstore`)
  - `/etc/checker.sh` (an MTK firmware installer; don't trigger it)
- The TV sometimes drops off the network briefly (Wi-Fi). Check `uptime` before assuming a reboot.

## Hardware and OS

| | |
|---|---|
| SoC | MediaTek MT5398, 2x Cortex-A9 @ ~1 GHz, NEON, VFPv3 (no VFPv4) |
| RAM | 726 MB |
| GPU | PowerVR SGX 544MP (`pvrsrvkm`, DDK `1.9@2291151 cobra_lg_linux`); `/dev/mali` is a stale node |
| Kernel | Linux 3.0.13 (gcc 4.5.1), built 2017-05-20, `panic=1` |
| libc | glibc 2.12.2, ARM EABI5 **soft-float** (`ld-linux.so.3`). Build with `-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=softfp` |
| Root fs | squashfs, read-only; many squashfs/unionfs mounts under `/mnt` |
| Writable flash | ext4 partitions: `/mnt/flash/data`, `/mnt/lg/cmn_data`, `/mnt/appstore`, `/home`, `/mnt/system_app` (don't touch) |
| Load avg ~40 | harmless: MTK kernel threads in D state; CPU ~80% idle |
| kexec | not compiled in (only the weak `sys_kexec_load` stub) |
| Watchdog | no `/dev/watchdog` in Linux |

Boot: the bootloader passes `root=/dev/mmcblk0p16 rootfstype=squashfs lginit=/dev/mmcblk0p17 bootmode=auto`.
`/etc/rc.d/rc.local` runs `/mnt/lg/lginit/lginit` in auto mode. The root telnetd on port 23 starts at
every boot. A second telnetd on port 2323 runs as user `browser`.

Tools on the TV: BusyBox 1.18.5 (**no** `sort`, `md5sum`, `nc`, `wget`, `losetup`), plus `gdb`, `perf`,
`dfbinfo`/`dfbdump`, `gst-launch` 0.10, `gxtest`, `galtest`, `/mnt/lg/res/lglib/eglinfo`.
The shell's line editor truncates input lines longer than about 1 KB.

## Software stack

- `procmgr` (`lg.service.special.processmanager`, `...processmanagergx`) launches and manages apps and
  assigns windows. `lginit_app` (pid 549 at boot) is `tmgr`, the debug-framework task manager.
- Services are declared in `/usr/local/bin/svclist*.rc` (types `tv|mgr|core|svc|appsvc`).
- `fxui`: Adobe Flash (Stagecraft) running LG's home UI (`/mnt/addon/contents/system/home/Home.swf`).
- `lb6wk`: LG's WebKitGTK 3.0 browser on GTK3, cairo and DirectFB 1.2. Runs NetCast HTML5 apps (the SDK's target).
- A Chromium build (`/mnt/webbrowser/chrome`), plus native add-ons in `/mnt/addon` (Netflix, Vudu, Skype, AllJoyn).
- IPC: D-Bus system and session buses, wrapped by LG's `libscf` (`SCF_BUS_*`).
  - The session bus address is in the app environment (`DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-...`).
  - The system bus socket is in `/usr/local/var/run/dbus`.
- Graphics:
  - Everything goes through `/dev/mtal` (MediaTek MTAL; `/dev/gfx` is a symlink to it) and `/dev/pvrsrvkm`.
    Nothing opens `/dev/fb*` or `/dev/fusion*`.
  - EGL/GLES live in `/mnt/lg/res/lglib`. The window system module is `libpvrPVR2D_CobraNetCast40WSEGL.so`.
  - **NetCast 4.0 mode (the default):** windows come from LG's HOA GX API in `libafopenapi.so`.
    `USE_NETCAST40=0` switches it to legacy `/dev/fb/N` mode instead.
  - DirectFB config is `/etc/directfbrc`: system=devmem, 1280x720 ARGB, sawman WM.
  - `libgal.so` and `libmtal.so` in `/mnt/vendor` are MediaTek's graphics/AV layers; apps `LD_PRELOAD` them.
- App environment (`/proc/<fxui pid>/environ`):
  - `LD_LIBRARY_PATH` covers `/mnt/lg/res/lglib`, `/mnt/vendor`, `/usr/local/lib`, `/mnt/addon/lib` and more.
  - `LD_PRELOAD=/lg/res/lglib/liblibm.so:/mnt/vendor/libgal.so:/mnt/vendor/libmtal.so`.
  - Crash hooks: `HOOK_SEGV=YES`, `CRASH_FRAMEWORK=YES`.
  - `PM_EXECPID`, `PM_SHELLPID`, `PM_EXECPATH`, `PM_EXECARGS`.

## Native GUI recipe (working: `src/nctest/`)

Reverse-engineered from `libafopenapi.so`, `gxtest` and the WSEGL module; LG ships no headers.

1. **Environment:** copy it from a running `fxui`, minus `PM_*`, `HOOK_*`, `CRASH_FRAMEWORK`,
   `GLIBC_BACKTRACE`, `LOAD_SYM`, `LOAD_DSYM`, `GDB_ROOT`, `LOCKUP_LEVEL`, `ATTR_*`, `USE_OSAMM`, `OSAMEM_SIZE`.
   Then set:
   - `PM_EXECPID=$(pidof procmgr)`
   - `PM_SHELLPID=$$` (then `exec` the program, so the PID matches)
   - `PM_EXECPATH=<binary>`
   - `PM_EXECARGS="--service lg.service.normal.<name>"`

   `HOA_PROC_RegisterService` parses **`PM_EXECARGS`, not argv**, whenever it's set.

   Also use `ulimit -c 0` and run with the working directory in `/tmp`.
2. **Load libraries** with `dlopen(RTLD_LAZY|RTLD_GLOBAL)` in this order:
   `libdbus-1.so.3, libsqlite3.so.0, libexpat.so.1, libstdc++.so.6, libdbgfrwk.so`.
   **Call `dbgfrwk_init(1, 0)` immediately**: its hook table is otherwise NULL and a later dlopen
   jumps to address 0. Then load `libafcommon.so` (defines `gendprog`, needed by `libscf`),
   `libscf.so`, `libafopenapi.so`, `libEGL.so`, `libGLESv2.so`.
3. `libdbgfrwk` installs its own SIGSEGV/SIGBUS/... handlers and **hijacks stdout/stderr**.
   Re-install your own signal handlers after every load, and log through a `dup(1)` taken at startup.
4. `HOA_PROC_RegisterService(argc, argv, &cb)`, where
   `cb = { int (*msg)(void*); int (*key)(int,int,int,int); int (*mouse)(int,int,int,int); }`.
   In `gxtest`, msg returns 0 and key/mouse return 1. Returns 0 on success.
5. `HOA_GX_GetWindowID(uint32_t *wid)` gets the window `procmgr` assigned (we got 26; the home UI was 27).
   `HOA_GX_CreateWindow` is a stub that always returns -6.
6. EGL: `eglGetDisplay(0)`, choose an RGBA8888 config with WINDOW_BIT and ES2_BIT, then
   **`eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)wid, NULL)`**. The native window is the HOA
   window ID. You get a 1280x720 surface.
7. Make it visible: `HOA_GX_GetWindowProperty(wid, prop)`, set `prop[1] = 1` (bVisible),
   `HOA_GX_SetWindowProperty(wid, prop)`, then `HOA_GX_RaiseWindow(wid)`. Calling `HOA_GX_UpdateWindow(wid, 0)`
   after the first swap is harmless.
8. GLSL ES: one uniform must not appear in both stages at different precisions, or the link fails.

Other signatures:
- `HOA_GX_GetVisibleWindowList(uint32_t **list, uint32_t *n)`
- The window property struct is about 48 bytes: word0 is some ID/flags, word1 is bVisible.

The window ID comes from `procmgr` over D-Bus (`/hoa/proc`, iface `lg.if.service`,
dest `lg.service.special.processmanagergx`), keyed by pid, service name and `PM_EXECPATH`.

## Toolchain and chroot

- Toolchain: [openlgtv/buildroot-nc4](https://github.com/openlgtv/buildroot-nc4) release `2026.08-webos`,
  asset `arm-lgtv-linux-gnueabi_sdk-buildroot-x86_64.tar.gz` (the NetCast variant).
  - Unpacked and relocated in `toolchain/`.
  - GCC 16.2, default `-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=softfp`, kernel headers 3.0.13, glibc 2.12.2.
- **The SDK's own glibc segfaults every dynamic binary on the TV.** Run against LG's glibc
  (copied in `tv_dump/glibc/`), but keep the SDK's newer `libgcc_s` and `libstdc++`.
- glibc 2.12 lacks `syncfs`, `setns` and `sys/random.h`. BusyBox needs `FEATURE_SYNC_FANCY`, `SEEDRNG`
  and `NSENTER` disabled.
- `clock_gettime` needs `-lrt`. Check output with `readelf -V` (stay at `GLIBC_2.4`..`2.12`).
- SDK sysroot extras: Python 3.14 (has `ctypes`; no `ssl`/`zlib`/`sqlite3`/`curses`/xml modules),
  SDL2 2.32 (dummy video driver only), curl 8.22 with OpenSSL 3.6, sqlite 3.53, GStreamer 1.24.

Chroot on the USB stick (`/mnt/usb/usb2/Drive1/netcast/`):
- `enter.sh ['cmd']` loop-mounts `netcast-root.ext4` (1 GB, ext4 with only features kernel 3.0 supports)
  at `/tmp/ncroot`.
  - It binds `/dev`, `/dev/pts` and `/tmp`, mounts `proc`, `sysfs` and a tmpfs on `/run`.
  - Read-only TV dirs are bound at their original paths: `/mnt/lg/res/lglib`, `/mnt/vendor`,
    `/mnt/addon`, `/mnt/browser`, `/usr/local`, plus the D-Bus socket directory.
  - No TV flash partition is mapped in.
- `teardown.sh` unmounts everything.
- Rebuild on the host with `build/build-image.sh`; output goes to `usb/netcast/`.
- Nothing persists across power cycles: re-run `enter.sh` after each boot.
- The `target/` folder already on the stick is an older Buildroot 2022.11.1 rootfs (Python 3.10) plus `ezinject`.
  It works only with the TV's `/lib/ld-linux.so.3`.

## Host to TV transfer

The host is a VM behind NAT (192.168.122.x): it can reach the TV, but the TV can't reach the host.

- `tools/tv.py 'cmd' [timeout]` runs a command over telnet and prints the output.
- `tools/pull.sh /remote/file localdir` copies TV to host (base64 over telnet).
- `tools/push.sh <subdir> files...` copies host to TV. It starts BusyBox `tcpsvd ... ftpd -w` confined to
  `/mnt/usb/usb2/Drive1/netcast` on port 2121, uploads with curl, kills it, and compares MD5s.
- Use the static BusyBox on the stick (`netcast/busybox`) for `md5sum`, `sort` and `losetup`.

## Possible GUI routes (ranked)

1. **Native EGL/GLES2 via HOA GX.** Working now.
2. Legacy framebuffer EGL (`USE_NETCAST40=0`, `/dev/fb/N`). Untested.
3. HTML5 apps with the SDK in `lb6wk`, optionally with a native helper process on localhost.
4. DirectFB 1.2. Possible but fragile.

## Input (verified 2026-11-19)

- Remote buttons arrive as evdev events on **`/dev/input/event2` ("LGE RCU")**: arrows `KEY_UP/DOWN/LEFT/RIGHT`,
  OK = `KEY_ENTER` (28), volume `KEY_VOLUMEUP/DOWN` (115/114), channel up = `KEY_CHANNELUP` (402).
  `lginput` injects them via `/dev/uinput`; `procmgr` reads `event0..5`.
- Other nodes: event0 "LGE KEY RETURNPATH", event1 "LGE CURSOR RETURNPATH" (pointer), event3 "LGE NLP",
  event4 "Smart Remote RCU Input", event5 "LGE Smart Remote - TouchPad", event6 "LG Magic gamepad" (js0).
  USB keyboards/mice show up as new `eventN` nodes; libnc rescans every 2 s.
- **The HOA key/mouse callbacks never fire for our app.** `procmgr` doesn't route input to a self-launched
  service, so read evdev directly. Reading evdev doesn't grab the device, so LG's UI behind us may react to
  the same presses. Not solved yet: becoming the "focused app", or `EVIOCGRAB`.
- `HOA_PROC_SetReady()` takes no arguments; calling it coincided with a crash, so libnc only calls it if
  `NC_SET_READY` is set. The HOA message callback's argument isn't a safe pointer: never dereference it.
- `HOA_PROC_UnregisterService()` returns -1 (harmless).

## Display resolution (verified 2026-11-19)

- **The panel is 1920x1080, but the graphics plane is 1280x720**, upscaled 1.5x in hardware (LG's UI too).
  `HOA_GX_GetWindowMaxResolution` = 1280x720, `DispRect` = 1920x1080, `fb0` = 1280x720 32 bpp double-buffered
  (`smem` 14.7 MB). `HOA_GX_SetWindowResolution(wid, 1920, 1080)` returns -1. Target 1280x720.
- 1-pixel detail doesn't survive the 1.5x scale: the striped test pattern looks broken on screen.
- PowerVR: upload 1-byte (GL_ALPHA) textures with 4-aligned widths. nctext uses a padded 1024x1024 glyph atlas
  with light hinting.

## libnc and apps (`src/`)

- `src/libnc/`:
  - `nc.c`/`nc.h`: runtime (HOA registration, window, EGL, evdev + HOA input queue, logging, crash guard).
  - `ncgfx`: GLES2 2D rects, textures and images.
  - `nctext`: FreeType text, default font `/mnt/lg/res/lgfont/LG_Display.ttf`.
- `src/apps/`:
  - `ncinput`: input probe.
  - `nctext`: on-screen event console and menu.
  - `ncres`: resolution probe and test pattern.
  - `sdldemo`: a plain SDL2 app.
  - `run-app.sh <app> [--service NAME] [args]`: the launcher.
- `cd src && make && make deploy`. Deploy goes to `/mnt/usb/usb2/Drive1/netcast/apps` via the temporary ftpd.
- Run on the TV: `sh /mnt/usb/usb2/Drive1/netcast/apps/run-app.sh /mnt/usb/usb2/Drive1/netcast/apps/<app> [args]`.
- EGL/GLES2 are linked against the TV's libs (no SONAME, so NEEDED is `libEGL.so`/`libGLESv2.so`) through
  `src/out/stubs` and `-Wl,--allow-shlib-undefined`. FreeType, zlib and bzip2 are linked statically from the SDK.

## SDL2 port (working)

- SDL2 2.32.10 with a new **`netcast` video driver** (`src/video/netcast/SDL_netcastvideo.c`), with libnc
  compiled in (`NC_NO_EGL`). Patch: `third_party/patches/sdl2-2.32.10-netcast.patch`.
  Build: `third_party/build-sdl2.sh`, output `third_party/sdl2-netcast/` (static `libSDL2.a`).
- One fullscreen 1280x720 window. SDL's EGL helpers create the surface on the HOA window ID.
  `SDL_Renderer` picks `opengles2`; `sdldemo` runs at 56.5 fps.
- Input: evdev keys go through `SDL_GetScancodeFromTable(SDL_SCANCODE_TABLE_LINUX, code)`; text input is
  US layout; REL_X/Y become relative mouse motion, REL_WHEEL becomes the wheel, BTN_* become mouse buttons.
  `SIGTERM` becomes `SDL_QUIT`.
- `SDL_Log` is routed through `nc_log` (dbgfrwk swallows stderr). `NC_TIMEOUT=<s>` sets a hard kill timer.
- No audio yet (ALSA/Pulse disabled; only OSS/dummy). No SDL_ttf/SDL_image in the static build yet.
- Host CMake comes from Kitware's prebuilt tarball in `third_party/` (no system cmake, no python venv).

## Audio (working)

- No ALSA playback (only capture nodes `pcmC0D0c`/`pcmC1D0c`). PCM goes through MediaTek's GStreamer 0.10 element
  **`mtkaudiosink`** (in `libgstmtkelements.so`): `render-mode=1` = real output, 0 = fake (silent, default),
  2 = "dump" (don't use). Accepts `audio/x-raw-int` at 6–192 kHz.
- SDL audio driver `netcast` (`src/audio/netcast/SDL_netcastaudio.c`): dlopens GStreamer 0.10 and runs
  `appsrc ! mtkaudiosink render-mode=1`, paced about 100 ms ahead of the wall clock.
- **Flash hazard:** LG's env has `GST_REGISTRY=/mnt/flash/data/gst_registry.arm.bin`. `run-app.sh` copies it to
  `/tmp/ncapp-gstreg.bin` and sets `GST_REGISTRY_UPDATE=no`; the driver also refuses non-`/tmp` registries.
  For manual gst tools: `GST_REGISTRY=/tmp/x.bin`, and BusyBox `timeout -t N`. There's no `audioconvert` plugin.
- Seen once: a segfault during audio/OPL init in Doom, not reproduced in later runs. Not yet investigated.

## Display alpha and SDL window gotchas

- The TV composites our plane over video and LG's UI using **per-pixel framebuffer alpha**. Partial alpha shows
  the HDMI picture through; alpha 0 (Doom's palette conversion) makes the window invisible.
  - SDL driver: forces alpha = 1 before each swap (`NC_ALPHA_OVERLAY=1` opts out).
  - ncgfx: uses `glBlendFuncSeparate(..., GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`.
- `SDL_CreateRenderer` recreates non-GL windows (Hide + Create). The driver re-shows the HOA window in
  `CreateWindow`; this was Doom's "sound but no picture" bug.
- libnc restores stdout/stderr after LG init, so apps' `printf` works. `NC_DEBUG=1` logs window calls and pixel
  readback at frames 1/60/300.
- Render targets (FBOs) work at all tested sizes (`src/apps/sdltarget.c`). `src/apps/sdlchoco.c` reproduces
  Chocolate Doom's video sequence.

## Doom (working)

- Chocolate Doom 3.1.1 (CMake, `ENABLE_SDL2_NET=OFF`) + SDL2_mixer 2.8.2 (static, WAV only) + Freedoom 0.13.0.
  Build trees: `third_party/doom-build`, `third_party/mixer-build`.
- Remote keys: the SDL driver maps CH+/CH- to PageUp/PageDown and Red/Green/Yellow/Blue to F1–F4.
  Doom cfg: fire=Enter, use=PgUp, menu=PgDn, confirm=Enter, strafe=F1/F2, run=F3, automap=F4
  (F-key menu shortcuts disabled).
- `run-app.sh` sets **HOME/XDG to `netcast/home` on USB** (LG's HOME=/home is flash). Doom gets
  `-config/-extraconfig/-savedir` in its own folder.
- Run: `sh /mnt/usb/usb2/Drive1/netcast/apps/doom/doom.sh`. (Correction 2026-11-20: Back does reach apps, as KEY_PREVIOUS 412; see Remote keys.)

## NetCast+ package (launcher + apps)

- `build/package.sh` builds `dist/netcast/` and **`dist/netcast-plus.zip`** (about 12 MB, FAT-friendly).
  Unzip to the root of a FAT32 stick, then run `sh /mnt/usb/usb2/Drive1/netcast/start.sh` from telnet.
  It detaches with nohup and logs to `/tmp/netcast.log`.
- `start.sh` loops: `system/launcher` (tiles from `apps/*/app.ini`) -> the chosen app's `exec=` script -> launcher.
  Only one process owns the HOA window at a time.
- `app.ini` keys: `title=`, `desc=`, `exec=`, `color=` (hex RGB), `order=`.
- Still needed: autostart without telnet (needs a persistent hook = flash write, excluded); Magic Remote
  pointer; gamepad; more apps.

## More apps / screen control (2026-11-20)

- **OpenTyrian** v2.1.20260913: plain Makefile, built with `SDL_CPPFLAGS/SDL_LDLIBS` overrides against the static
  SDL2. Data is `tyrian21.zip` from camanis.net (freeware release; the zip still contains the 1995 EULA).
  Bundled only with `build/package.sh --with-tyrian-data`. Preset `opentyrian.cfg`: fire=Return,
  change fire=F1, sidekicks=F2/F3, Scale2x, 4:3.
- ~~The SDL driver maps CH- to Escape~~ (superseded 2026-11-20: Back = Escape, CH- = PageDown). Doom's cfg uses 27 for menu, back and abort.
- Screen off with Wi-Fi kept: there's no direct HOA setter (`HOA_CTRL_IsScreenoff` only). LG's
  *Picture -> Energy Saving -> Screen Off* does it. Candidates for a tool: `HOA_CTRL_SetPicControl` (enum values
  unknown; could change saved picture settings), and MTAL `MTPMX_PANEL_EnableLVDSPower` /
  `MTPMX_PANEL_SetBackltDimming` (direct hardware). The network dropouts were the TV going to standby: disable the
  sleep/auto-off/no-signal timers in LG's TIME menu.
- The project README has the user's Doom-on-TV video (`docs/media/`: H.264 720p, poster, GIF, original HEVC).

## Screen off, keep-awake, services (2026-11-20)

- **Backlight = LG MICOM, not MTK.** `MTPMX_PANEL_EnableBacklightPower` (ioctl `0x40040D09`) does nothing visible.
  LG's path in RELEASE: `API_DDM_BackLight_OnOff(on)` -> `DDI_GPIO_SetControl(25 "EnableLCDDisplayPanel")` ->
  MICOM GPIO `0x2102` -> `DDI_MICOM_GPIO_SetOutputPort`: I2C write `{0x36 on | 0x37 off, 0, 0}` to the MICOM.
  - The MICOM is LG I2C ch 4 = MTK SIF bus 1, address 0x52, clkdiv 380. Captured live with `src/tools/sniff.c`.
  - SIF ioctls on `/dev/mtal`: write `0x40200C0B`, read `0x40200C0C`, with struct
    `{bus, clkdiv, dev, subaddr_len, subaddr, buf*, count, pad}`.
  - `src/apps/ncscreen.c`: `on|off`, `off --wake`, and `saver [min]` (idle daemon). Verified on screen;
    Wi-Fi stays up. LG's own Picture->Energy Saving->Screen Off seemed to take the network down.
  - The ack read returns 0xff from our process, but the command works.
- **Sleep timers:** RELEASE keeps settings in a RAM shadow at `0x03148440`. Auto Off is `+0x936D` (15-min timer
  `0x4000000B`), No-signal power off `+0x937E`, No-operation standby `+0x9380`. Menu setters write the byte and then call
  `UI_SVC_REGISTRY_WriteNVRAM` (flash). `src/tools/ncnosleep.c` pokes the bytes to 0 via ptrace (RAM only) after
  verifying firmware code words. On this TV, Auto Off and No-signal were ON: the cause of the "TV went to sleep" episodes.
- `/proc/<pid>/mem` is not readable on 3.0 without ptrace attach. `src/tools/peek.c` reads words via ptrace.
- RELEASE (19 MB) is not stripped: its full symbol table makes RE practical. A copy is in `tv_dump/release/`.
- **Services:** `HOA_PROC_DisableAndStopServices(const char **names, int n)` overflows its own heap buffer for long
  lists, so call it once per name (`src/apps/nclite.c`). LG's `svclist_gip.rc` itself disables myappsmini, voicenlp
  and webbrowser. nclite still needs a live test.
- Every LG app maps `/mnt/lg/cmn_data/appfrwk_mask` (flash) MAP_SHARED rw; run-app.sh points `APPFRWK_MASK_PATH` at `/tmp`.
- One crash log of ours exists on flash: `/mnt/flash/data/exc_log_nctest_pid2379.txt` (1.9 KB), from the first
  nctest crash before our handlers were reinstalled after each dlopen. Left in place: deleting it would also write flash.

## Media player, hardware decode (2026-11-20)

- GStreamer 0.10 `playbin2` with `video-sink=mtkvideosink` and `audio-sink=mtkaudiosink render-mode=1` plays through
  the MediaTek decoders. LG's patched playbin hands H.264 straight to the sink, which decodes it on the video plane.
  The OSD is cleared to alpha 0 so the video shows through, with an info bar drawn on top (`src/apps/ncplay.c`).
- Elements present: `omx_h264dec`, `omx_mpeg2dec`, `omx_mpeg4dec`, `omx_aacdec`, `omx_mp3dec`, `omx_ac3dec`;
  qtdemux, matroskademux, mpegtsdemux, flv; souphttpsrc, rtspsrc, udpsrc. **Not working:** raw AAC in MP4
  ("Missing element: MPEG-4 AAC decoder"; ADTS in TS is fine) and HLS (no hlsdemux).
- The TV has no internet route (gateway unreachable, DNS proxy fails). For tests, serve media on the TV itself:
  chroot `python3 -m http.server 18080 --bind 127.0.0.1` (LG uses 8080).
- Bus messages are polled with `MSG_TYPE(m) = *(int*)((char*)m + 24)` (GStreamer 0.10 GstMessage layout).
- VLC/mplayer would decode in software on 2x A9: fine for audio and SD, not for HD H.264. The hardware path is the
  one to use.

## RetroArch (2026-11-20)

- RetroArch v1.22.2: `--enable-opengles --disable-opengles3 --enable-sdl2 --enable-egl`. Video `gl` plus context
  `sdl_gl` on our SDL2. libstdc++ must be static (`-l:libstdc++.a` in Makefile.common/config.mk, `LINK=g++`):
  the TV's libstdc++ lacks newer GLIBCXX versions.
- **getopt crash:** HOA_PROC_RegisterService calls `getopt_long`, which crashed after RetroArch had parsed its own
  arguments. libnc now saves `optind`/`opterr`, sets `optind = 0` (full re-init) around registration, and restores them.
- run-app.sh does `cd /tmp`, so the app's paths must be absolute (`:/` paths in retroarch.cfg are relative to the
  executable). `--menu` is added only if no content is given.
- Cores (`third_party/retro/build-cores.sh`, flags in CC/CXX because the cores' Makefiles override CFLAGS):
  fceumm, snes9x2005, gambatte, genesis_plus_gx, gpsp (`CPU_ARCH=arm HAVE_DYNAREC=1`), pcsx_rearmed
  (`DYNAREC=ari64 HAVE_NEON=1 BUILTIN_GPU=neon`). The bundled test content is the 240p Test Suite (GPL-2.0).
- Remote mapping in retroarch.cfg: A=OK(Enter), B=CH+(PageUp), X/Y=yellow/blue(F3/F4), Start=green(F2),
  Select=red(F1), menu=CH-(Escape).

## Settings app (LVGL 9.6, 2026-11-20)

- `src/apps/ncsettings.c`: LVGL 9.6.0 on the SDL2 netcast driver (`lv_sdl_window_create(1280,720)`,
  `lv_sdl_keyboard_create`, `lv_sdl_mouse_create`, `lv_tick_set_cb(SDL_GetTicks)`). LVGL is built into
  `out/liblvgl.a` from `third_party/lvgl` with `src/lvgl/lv_conf.h` (`LV_CONF_INCLUDE_SIMPLE`).
- LVGL's SDL keyboard driver maps Escape (= CH-) to `LV_KEY_ESC` and PgUp/PgDn to PREV/NEXT. UP/DOWN are handled in
  the app (focus prev/next unless a dropdown is open).
- Settings are in `netcast/home/settings.conf` (`SAVER_MIN`, `NOSLEEP`, `LITE`). They're applied immediately and again
  by start.sh at boot. Lite mode runs nclite through run-app.sh, because nclite needs its own HOA registration.

## Terminal (2026-11-20)

- `src/apps/ncterm.c`: libvterm 0.3.3 (MIT, `out/libvterm.a`) with a `forkpty` shell, rendered with libnc/ncgfx/nctext
  using DejaVu Sans Mono 22 px. It runs the chroot (`netcast/enter.sh`, which resets the environment but keeps `TERM`)
  if present, otherwise `/bin/sh -l`. USB keyboard with US layout; the remote's CH- sends ^C and CH+ sends Tab.
  `TERM=xterm-256color`.

## Remote Desktop: FreeRDP 3 (2026-11-20)

- FreeRDP 3.33.1, SDL2 client, fully static except glibc (9.6 MB stripped). Notes for the cross build
  (`third_party/freerdp-build`):
  - The SDL2 client needs **SDL2_ttf** for its dialogs. It's built static (2.24.0, system FreeType, no HarfBuzz)
    into `sdl2-netcast/`.
  - glibc 2.12 has no `<threads.h>`: build with `-D__STDC_NO_THREADS__`, so `WINPR_TLS` becomes `__thread`.
  - LTO is on by default and needs `CMAKE_CXX_COMPILER_AR`, which the SDK lacks: `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF`.
  - Pass `FREETYPE_LIBRARY` as a single path. bz2 and zlib go in `CMAKE_CXX_STANDARD_LIBRARIES`.
  - `-latomic` (from the OpenSSL static deps) resolves to a `libatomic.so` that is really `libatomic.a`, placed
    first on `-L`: the TV has no libatomic.so.1.
  - Options: OpenSSL 3.6.4 static from the SDK; no X11, FFmpeg, ALSA/Pulse, CUPS, PCSC, Kerberos, FUSE, JSON or AAD.
- The netcast SDL driver sends real scancodes, which the FreeRDP SDL client needs for keyboard input.
- `apps/rdp/rdp.conf`: one line of arguments (`/v:HOST /u:USER`). run.sh adds
  `/size:1280x720 /f /cert:tofu /gfx /sound:sys:fake +clipboard`. The password prompt is FreeRDP's SDL dialog.
- Not tested on the TV yet. Open questions: the mouse cursor (our driver has no hardware cursor) and audio (no
  SDL rdpsnd backend; sound is disabled with `fake`).

## Kernel modules (2026-11-20)

- LG's GPL tarball for this model wasn't found. Vanilla **linux-3.0.13** (kernel.org, sha256 checked) plus the Bootlin
  armv7-eabihf 2018.11 toolchain (GCC 7.3; `compiler-gcc7.h` copied from gcc4) builds modules whose vermagic
  matches exactly: `3.0.13 SMP preempt mod_unload ARMv7`.
- Config: vexpress_defconfig plus SMP, PREEMPT, MODULE_UNLOAD, AEABI, VFP/NEON, **VMSPLIT_3G (PAGE_OFFSET 0xC0000000)**,
  and no MODVERSIONS (the TV's modules have no `__crc_`). Build with `make -C src/kmod`
  (`KCFLAGS=-fno-pie -fno-pic`).
- Built: `nchello.ko` (log only) and `xpad.ko` (Xbox pads). Our `struct module`
  (`.gnu.linkonce.this_module`) is 0x140 bytes, with init at +0xbc and exit at +0x138.
- **Before any insmod:** pull one of LG's `.ko` (e.g. `/mnt/lg/res/lglib/tfat.ko`) and compare that size and those
  offsets. LG's config may add fields to `struct module` (e.g. tracepoints, kmemleak), and a mismatch crashes the
  kernel. If the layout matches, the first test is `insmod nchello.ko` then dmesg, from RAM only (`/tmp` or the stick).

## Qt 6 / QML (in progress, 2026-11-20)

- Qt 6.12.0 (qtbase, qtshadertools, qtdeclarative). Host tools are built in `third_party/qt/host`
  (`build-host.sh`). The target is a **static** build (`build-target.sh`): eglfs, GLES2 against LG's EGL/GLES
  (stubs + `--allow-shlib-undefined`), evdev input, bundled FreeType/HarfBuzz/PCRE/zlib/libpng; no fontconfig, udev,
  D-Bus, network or widgets.
- **`eglfs_netcast`** device integration (added to the qtbase tree, enabled by `-DNETCAST_LIBNC_DIR`):
  - `platformInit` calls `nc_init` with `no_egl`/`no_evdev`.
  - `createNativeWindow` returns `nc_window_id()`. `presentBuffer` calls `nc_update_window` once.
  - 8888 surface, 1280x720, 96 dpi.
  - It never opens `/dev/fb0`: the base eglfs `platformInit` would send it `FBIOBLANK`.
- App: `src/qml/hello`. Env: `QT_QPA_PLATFORM=eglfs QT_QPA_EGLFS_INTEGRATION=eglfs_netcast QSG_RENDER_LOOP=basic
  QT_QPA_FONTDIR=<fonts>`. Qt's evdev keymap already maps KEY_CHANNELDOWN to Qt::Key_ChannelDown.

## Network Shares (SMB) and Music (2026-11-20)

- **libsmb2 6.3** (LGPL-2.1, static, no Kerberos): SMB2/3 with NTLM, so no kernel `cifs` module is needed.
  `third_party/smb2-netcast`. SMB1-only servers are not supported. 7.0.0 came out the day before, so 6.3 was used.
- `src/apps/smbcommon.c`: `shares.conf` (`Name = smb://[DOM;]user[:pass]@host[:port]/[share[/folder]]`,
  percent-escapes decoded) and connect/share enumeration (`smb2_share_enum_async` on IPC$; hidden, `$` and
  non-disk shares filtered).
- **`smbgw`** (`src/apps/smbgw.c`): read-only HTTP gateway on 127.0.0.1:8445.
  - Routes: `/s/<entry>/<share>/<path>` and `/l/<path under /mnt/usb>`.
  - Directories come back as an HTML link index, the format Photos already parses. Files support HEAD and Range.
  - One SMB connection per share, mutex-shared, reconnected once on error. `..` is refused, and shares.conf is
    reloaded when it changes.
  - This is how Photos, the hardware Player (souphttpsrc) and Music read SMB files without any SMB code.
- **Network Shares** (`ncsmb.c`, SDL2 + SDL2_ttf via `sdlui.c`): browses the shares and the USB stick, and hands
  files to the other apps. Music gets an m3u; Photos gets a conf with `url=` the gateway folder; the Player gets
  a gateway URL or `file://`.
  - Hand-off: the app writes `/tmp/ncmenu.next` (a shell script: the target app, then itself again), and start.sh
    runs it after the app exits. start.sh exports `NETCAST_SESSION=1`. Without it, the app runs the chain
    detached itself. The location is kept in `/tmp/ncsmb.state`.
- **Music** (`ncmusic.c`): dr_flac/dr_mp3/dr_wav (pinned dr_libs commit) on a decoder thread feeding
  `SDL_QueueAudio` (~0.5 s queued). SDL converts 24-bit, 96 kHz and >2 channels to the driver's S16 stereo at
  the nearest rate.
  - The reader is a local file or HTTP with ranges (64 KB buffer, reconnects on far seeks).
  - Tags: Vorbis comments and ID3v2.2–2.4 parsed by hand from dr_mp3's raw metadata. Cover: FLAC PICTURE / ID3
    APIC (front cover preferred), else cover.jpg/folder.jpg next to the file. stb_image, box-downscaled to ≤512 px.
  - dr_libs gotcha: `drflac_open_with_metadata`/`drmp3_init` give the I/O and metadata callbacks one shared user
    pointer, so the metadata target hangs off the reader.
  - `NCMUSIC_SELFTEST=1 ncmusic <items>` decodes everything headlessly (format, tags, cover, full decode, seek).
- **Tested:** on the host against impacket's SMB2 server (auth, share list, listings, ranges, 4 parallel streams
  byte-exact, traversal 403, missing file 404). On the TV: ARM decode >10x real time, gateway local route,
  Music UI with audio (user heard it), and the browser -> Music -> browser hand-off. Not yet tested on the TV
  against a real SMB server.
- Test-only: `NC_KEYS="Down,Return"` makes sdlui apps press keys (one every 0.7 s), and it's not passed on to
  apps they start.
- Gotcha: the TV's BusyBox has no md5sum or wget; use `netcast/busybox`. Also, `pkill -f <name>` from a shell
  whose own command line contains <name> kills that shell (use `pkill -x` or a `[n]ame` pattern).
- Host disk: the Qt Quick host build alone used ~9 GB (PCH). Rebuild with `-DBUILD_WITH_PCH=OFF` and fewer modules.

## Remote keys: Back and volume (2026-11-20)

- Input devices: event0 "LGE KEY RETURNPATH", event1 "LGE CURSOR RETURNPATH", event2 "LGE RCU", event3 "LGE NLP",
  event4 "Smart Remote RCU Input", event5 "LGE Smart Remote - TouchPad", event6 "LG Magic gamepad".
- Captured with `src/tools/evdump.c` (read-only). All on event2: **Back = 412 (KEY_PREVIOUS)**, Vol+ 115,
  Vol- 114, Mute 113, OK 28. The earlier "Back never reaches apps" was wrong: it was just unmapped.
- libnc maps 412 to KEY_ESC for every app. The SDL driver maps Escape, so Back is back/menu everywhere, and CH-
  is now PageDown. Photos and Player no longer treat CH- as back; the terminal keeps CH- = Ctrl-C.
- **Volume:** while our app has focus, LG doesn't act on volume keys. libnc calls LG's API from libbcopenapi
  (loaded lazily), with the calling convention taken from RELEASE's own caller `UI_GESTURE_CtrlSetVoluem`:
  `HOA_CTRL_SetVolume(0 /*relative*/, 2 /*type*/, 1, ±1, &newvol)`. The RS-232 handler uses
  `(1 /*absolute*/, 2, 0, level, NULL)`. `HOA_CTRL_GetCurrentVolume(2, &u8)`. Mute is
  `HOA_CTRL_GetMute(&m)` + `HOA_CTRL_SetMute(!m, 1)`; that second argument is a guess. LG persists the level
  itself, as for any volume change. `NC_NO_VOLUME=1` disables this. Not yet tested on the TV.
  Qt apps (`no_evdev`) don't get this yet.
- RELEASE's key-name table at file offset ~0xdd6c14 is `{const char *name, uint32 linux_code}` pairs
  (IR_KEY_VOL_UP -> 115, IR_KEY_CH_UP -> 402, IR_KEY_EXIT -> 174): the IR-to-evdev conversion for "LGE RCU".
- `HOA_PROC_UnregisterKeysFromAppKeyTable(const uint32_t *keys, uint32_t n)` exists (it would hand keys back
  to LG). The key code space it uses is unknown, so it isn't used.

## Flash layout and secure boot (2026-11-20)

Dumped read-only to `tv_dump/flashdump/` (via BusyBox `dd` from flash to the USB stick, then BusyBox
`httpd` on the TV + `curl` from the host; nothing written to flash). `tools/tv-dump-flash.sh` reproduces it.

**Storage: eMMC only, no NAND/MTD.** `/dev/mmcblk0` ~3.8 GB, GPT-ish with 63 partitions plus
`mmcblk0boot0/1` HW-boot areas (no /dev nodes). `/proc/mtd` is empty. The partition map and names come
from `p2` (`PART.INFO`): mtk3598-emmc, boot(.bin), partinfo, mapbak, swue, env_nvm, widevine, idfile,
cert, authcxt, nvram, marlin, hdcp, rootfs, lginit, model, cfginfo, logo, kernel(uImage), lgapp(.lz4),
lgres, lglib, lgfont, opensrc, emanual, **tzfw(.bin)**, base, patch, skype, pqldb. There are **two banks**
(A/B): many partitions mirror each other; the inactive copies read as `0xFF` (erased).

**The system is squashfs, read-only.** `cmdline: root=/dev/mmcblk0p16 rootfstype=squashfs ... bootdev=emmc`.
rootfs and every LG system partition mount `squashfs ro`. The only writable areas are ext4:
p62 (shared `cmn_data`/`home`/`cache`/`user`/`flash/data`), p61 `system_app`, p63 `appstore`, p15 hdcp,
p12 ciplus, p31 vcs. (This is why the USB-overlay approach is the natural one.)

**No verified/secure boot of the system images. Evidence:**
- Preloader `p1` = MTK ROM/MSDC stub (`MTK/DTV/ROMCODE/MSDCBOOT/V01.00`): integrity is an **LZHS checksum**
  only, and it prints **"not secure loader"** (eFuse secure-boot bit not blown on this unit).
- u-boot env `p8` has **`verify=n`** (image CRC verification off), `autoboot=y`, `bootdelay=0`, `silent=y`.
- Kernel mounts the raw squashfs directly: **no `dm-verity`**, no `verity=`/IMA in cmdline. `modules_disabled=0`
  (consistent with our module work).
- Partition integrity is **CRC32** (`p2` `CRC32.INFO`), and nothing in the boot path reads it: not `rcS`,
  `checkroot.sh` (stock fsck), the `lginit` binary (no crc/verify/secure strings), nor anything in
  `/sbin /bin /usr /mnt/nsu`. CRC32.INFO is OTA-updater (`swue`) metadata, checked during an update, not at boot.
- TrustZone (`tzfw.bin`) and `cert` exist, but `p28` (a non-stripped ARM ELF object) is the **DRM** crypto
  stack — libtomcrypt 1.16, ECDSA/ECC-P256/AES/SHA, path `dmx9880/drm/...` (Widevine/Marlin/HDCP/secure
  storage), not rootfs/kernel authentication.
- Kernel images `p21`/`p22` are MTK **"LZ4P"**-wrapped; `p20` is an LZHS/"LZ" blob (logo/loader). No signature
  envelope around them.

> **CORRECTED 2026-11-20: the claim below is WRONG.** U-Boot RSA-verifies rootfs, lginit, kernel, lgapp,
> vendor, opensrc, lglib and tzfw on every boot. A failed check hangs the TV and, in RELEASE mode, burns a
> "full verify" eFuse. See "U-Boot boot flow and failure fallbacks" below. The evidence above missed it
> because U-Boot is LZHS-compressed inside p1, so its strings never showed up.

**(Superseded) So: a modified (unsigned) rootfs/kernel would boot — the boot chain does not reject it.** The brick risk is
**mechanical, not cryptographic**: (1) a squashfs partition is read-only and fixed-size, so persisting a change
means rebuilding the squashfs and reflashing a whole partition with `dd` to `/dev/mmcblk0pN` — a real flash
write; (2) a wrong-partition or interrupted/partial write to `p1`/`partinfo`/`tzfw`/env would brick it;
(3) the OTA updater may react to a `CRC32.INFO` mismatch on the next update (rollback/forced reflash —
usually recoverable, but unverified); (4) the A/B bank select logic isn't mapped yet. **Conclusion: persisting
changes is technically possible (no secure boot blocks it), but every risk is in the write itself and the
OTA/CRC layer — so we keep writing nothing to flash and stay on the USB overlay until the bank-select and
updater CRC behaviour are understood from the dumps.**

Dumped partitions (host `tv_dump/flashdump/`, md5-verified against the TV): p1 bl, p2/p3 PART.INFO (identical),
p6/p7, p8 u-boot env, p13 key/DRM blob, p16 rootfs, p17 lginit, p18 model, p20 LZ blob, p21/p22 kernel(LZ4P),
p28 DRM ELF.

## Bank-select and the swue updater (2026-11-20)

RE'd from `p2` (PART.INFO), `p4` (swue.squashfs, dumped & mounted read-only), and the SWUE binary
(`tv_dump/flashdump/SWUE`, `SWUE.sym`, `SWUE.cfg`).

**PART.INFO (p2, with a byte-identical backup `mapbak` = p3) is a logical->physical partition map,
96-byte records:**
- `+0x00` name (e.g. `rootfs`), `+0x30` filename (`rootfs.squashfs`).
- `+0x20`: **physical byte offset** and **size** on `/dev/mmcblk0` (u32 each). e.g. boot@0x0/1MB=p1,
  partinfo@0x100000/256K=p2, swue@0x180000/32MB=p4.
- `+0x40`: reserved, then at `+0x50` the **image size** (e.g. rootfs `0x89d500`; the squashfs reports `0x89b234` bytes used. Earlier notes wrongly called this field the CRC32), `+0x54` **version** (`0x04031001` = fw v04.03.10), `+0x58` **flags**, `+0x5c` **type**. Per-partition CRCs are kept only in `CRC32.INFO`.
- Header: magic `16 07 12 20`, device `mtk3598-emmc`.

**Bank select = the active-flag in PART.INFO, not a bootloader counter.** Updatable partitions have
**two records** (A/B). The active copy has **flags bit0 = 1** (`0x101`); the inactive has `0x100`.
e.g. `rootfs` active@0x37700000 (=p16, matches `root=/dev/mmcblk0p16`) vs inactive@0x5200000; same for
`kernel`, `lginit`, `swue`, and the LG app/res partitions. The bootloader reads PART.INFO, picks the
active record per logical name, and builds `root=` from its offset. CRC32 is stored
in a dedicated `CRC32.INFO` partition; `CFG.INFO`, `env_nvm`, `macadr` are separate.

**swue (Software Update Engine):** a separate 32 MB partition (p4), mounted only during an update
(normally `/mnt/swue` is empty; no swue process at boot). Key functions (from `SWUE.sym`):
`API_SWU_GetAvailPartIndex/CRC32`, `API_SWU_GetCurUsedPartIndex/CRC32`, `API_SWU_SetCRC32`,
`API_SWU_VerifyImage`, `## Boot Swap ##` / `Boot Swap Okay.`, `API_DDM_POWER_Shutdown_ByVerifyFail`,
`gCurEpkVer`/`gOldEpkVer`. Update flow:
1. Read an **EPK** package (`Decrypt signed & encrypted EPK`) — OTA images are **RSA-signed and
   AES-encrypted** (swue bundles `libcrypto.so.1.0.0` + `netflix_pub.pem`), so a valid EPK can't be forged.
2. Write images to the **available (inactive)** bank.
3. `API_SWU_VerifyImage`: CRC32 the freshly-written images; on mismatch it does **not** swap and can
   power off (`Shutdown_ByVerifyFail`).
4. `SetCRC32` into PART.INFO, update EPK version, then **Boot Swap** = flip the active-flag in PART.INFO
   and mapbak; reboot into the new bank.

**Implications for the brick question (updated):**
- CRC32 is an **update-time** integrity check, *under* the EPK signature. It is **never checked at boot**
  (swue doesn't run at boot; u-boot `verify=n`; no dm-verity; no boot script reads CRC32.INFO).
- So a locally-modified active partition **boots fine**; its PART.INFO CRC32 just goes stale and nothing
  cares. The stale CRC could matter only to a future OTA (`GetCurUsedPartCRC32`), but OTA needs a signed
  EPK over the internet this TV can't reach, so it's not a practical local-modding hazard.
- The "proper" persistent path mirrors swue: write the **inactive** bank, update its CRC32+active-flag in
  **both** PART.INFO (p2) and mapbak (p3), swap, reboot. Doing it by hand is the risky flash-write path;
  corrupting PART.INFO loses the logical->physical map (mapbak is the only backup).
- Net: still no secure-boot barrier to local mods; the brick risk stays mechanical (writing
  PART.INFO/bootloader/wrong offset, interrupted write) — so the no-flash-write rule holds.
  **(Superseded: there IS a boot-time signature barrier. See the next section.)**

## U-Boot boot flow and failure fallbacks (2026-11-20)

Work is in the session scratchpad: a Python LZHS decoder (port of openlgtv `epk2extract`), the decompressed
images and the disassembly. To redo it, LZHS-decode p1 at `0x80000` and disassemble at VMA `0x800000`.

**p1 (`boot.bin`) layout:** `0x0` MTK ROM preloader (MSDCBOOT, "not secure loader") · `0x9c00`/`0x9d00`
header plus 0x300 bytes of signature-like data · `0xa000` "X935" header, then LZHS at `0xa040` → **MT5398
"Lmain" loader** (0x52800 bytes, Thumb, base `0xd00000`, DRAM init) · `0x33480` "LGE"/`mt5398_LGE_linux`
tag · LZHS at `0x80000` → **U-Boot 1.3.4** (0x853e0 bytes, ARM, `TEXT_BASE=0x800000`, prompt
`NC4_M13 # `, built Jan 29 2014).

**Partition images are signed.** Each image = squashfs (4K-padded) + a **0x1500-byte trailer of 21 × 256-byte
RSA-2048 signatures** (checked on rootfs and lginit: high-entropy blocks follow the squashfs). Signatures
0–19 are "partial": signature k covers 4 KB blocks k, k+20, k+40, … Signature 20 covers the whole image.
The scheme is SHA-1 + RSA public decrypt (`0x8235f4`). The modulus is read from SRAM `0xfb005120`, a 4 KB
block that Lmain copies from SRAM `0xf000` (put there by an earlier boot stage; it is not stored in plaintext in p1).

**Boot sequence (`0x8006d0`, called from board init at `0x800dd8`):**
1. "fast boot check": unless the debug level is RELEASE, a 200 ms window where a backtick (`0x60`) on the
   UART drops to the U-Boot console.
2. Work out the boot state (`0x81bc64`): 0 = normal, 1 = cold boot that will make a snapshot, 2 = resume a
   snapshot (`lgsnap` on the cmdline; image in the 64 MB `hib` partition).
3. **`verify_apps(mode)` (`0x8239dc`)** walks a fixed table: lginit, rootfs, tzfw (both modes); vendor,
   opensrc, lglib (normal mode); patch, emanual, estreamer, extra and base only when full verify is on;
   swue only in SWU mode. With the OTP bit `0xf0008664` bit0 ("full verify") clear, it checks **one
   partial signature per partition, picked pseudo-randomly each boot** (`[0xf0008074] % 20`). Over many
   boots every block gets checked.
4. If SWU mode is set (I²C EEPROM: magic `0x20121205` at NVM `0x198` and byte `0x18b == 'S'`), it runs
   `cp2ram swue 0x04000000;verification 0 swue 0x04000000;` + the kernel command. That boots the update
   engine, which resumes an interrupted update.
5. Otherwise: `cp2ramz lz4 kernel 0x7000000 0x7FC0;verification 0 kernel 0x7000000;xiplz4 lgapp;bootm 0x7FC0`.
   bootargs are built from the **active** PART.INFO records (`root=/dev/mmcblk0p%d`, `lginit=`, …).

**What happens on each failure (the fallback question):**

| Failure | Behaviour |
|---|---|
| PART.INFO CRC bad (CRC over 0x3050 bytes, stored at +0x3050) | Copies **mapbak (p3) over partinfo (p2)** and carries on. This is a flash write by U-Boot itself. mapbak's own CRC isn't checked first. |
| Signature mismatch (rootfs/lginit/kernel/lgapp/tzfw/…) | Prints `signature check fail!`. In **RELEASE** it then **burns the full-verify eFuse** (`0x823394`, permanent) and **spins forever** (`b .` at `0x8236b4`). No reset, no bank fallback. |
| RSA decrypt error (bad padding) | Returns -2 and boot **continues**. This looks like an oddity, not a usable bypass. |
| Snapshot image bad | RELEASE: erases `hib`, then `reset` → cold boot (a real fallback). DEBUG: hangs. |
| Kernel load or bootm fails | The boot function returns, then `main_loop`. The saved env has **`bootcmd=` (empty)**, `bootdelay=0` and no `watchdog=yes`, so it **sits at the `NC4_M13 #` UART prompt**. The HW watchdog (`0xf0028100`) is only armed if `watchdog=yes`. |
| Kernel panic or init dies | cmdline has **`panic=1`**, so it reboots after 1 s into the **same** bank: an endless boot loop. No Linux watchdog device. |
| Interrupted OTA | The SWU flag is still set in the EEPROM, so the next boot re-enters swue and the update resumes. |

**There is no automatic A/B fallback.** The name→record lookup (`0x815448`) returns the first record whose name
matches and whose active byte (+0x58) is set. Nothing counts failed boots, and nothing retries the inactive
copy. Only swue flips banks ("Boot Swap"), after a successful update. The inactive records do still describe
a **previous firmware v04.03.09** for rootfs, kernel, lgapp, opensrc, base and vendor (non-zero image sizes, older
versions), so the old bank may still be intact. Even so, switching to it means editing PART.INFO and mapbak by hand.

**Recovery paths that exist (all need the UART console, which RELEASE mode locks out via the fast-boot
check):** `swum` (EPK from FAT32 USB), `swu` (EPK over TFTP), `loadm`/`load` (raw image to a partition),
`usbboot`, `partinfo` (edit the map), `setboot`, `swumode on`. SWUE also has a USB EPK path
(`USBUPDATE`, `MSG_SWU2UI_USB_UPDATE_*`), but EPKs are signed. SWUE shares the eFuse policy: "verify
failed in 2nd check ... OTP will be set and System will be shutdown".

**Implications:** reflashing a modified rootfs/kernel/lginit/lgapp **will brick the TV** (hang at boot) and
may permanently turn on full verification. Only partitions outside the verify table (e.g. `lgres`, `lgfont`,
`model`, `logo`, `cfginfo`, `pqldb`, `skype`, `tplist`, `preload`, `vocdb`) and the ext4 data partitions are
not signature-checked at boot. The no-flash-write rule is now a hard rule.
**Debug level (2026-11-20): this unit is RELEASE (2).** NVM byte `0x18a` (0 DEBUG / 1 EVENT / 2 RELEASE) lives in an
I²C EEPROM (LG NVM handle ch 3 / dev 0xA8 → MTK SIF bus 0, clkdiv 380, 2-byte big-endian subaddress), not on the
eMMC. U-Boot ignores it if the valid mark at `0x14` is `ff ff ff ff`, and treats a value > 2 as RELEASE. Only the
LG debug-status setters (plus the factory "clear NVM" debug commands) write it. SIF ioctl struct is
`{bus, clkdiv, dev, u8 *subaddr, subaddr_len, buf*, count, pad}` (field 3 is a pointer; `ncscreen.c`'s comment has
it the other way round, which is harmless there because it passes 0/0). Tool: `src/tools/ncdebug.c`
(`status`, `dump`, `set <level> [--yes]`, `toggle [--yes]`; dry run without `--yes`; addressing self-test,
one-byte write plus read-back verification). The level takes effect at the next cold boot.
DEBUG/EVENT: UART break-in, no full-verify eFuse burn on a signature failure, but a bad snapshot is not auto-cleared.
**Still unknown:** whether the full-verify OTP is already set (`0xf0008664` bit0, readable with `devmem`), and whether the MICOM
power-cycles a hung SoC.

## Next steps

- On the TV: test Settings, Terminal, Remote Desktop and QML; confirm the media player and RetroArch visually; live-test nclite.
- Kernel modules: compare `struct module` against an LG .ko, then insmod nchello, then xpad (gamepads), tun, cifs.
- Investigate the intermittent audio-init segfault; Wi-Fi stalls after large uploads (try Ethernet).
- Magic Remote pointer; ScummVM; a file browser.
- Newer kernel: research only (no kexec; LG's display/GPU/Wi-Fi drivers are binary modules for 3.0.13).

## Project layout

| Path | Contents |
|---|---|
| `src/nctest/` | first GUI probe (`nctest.c`) and launcher (`run.sh`) |
| `src/libnc/`, `src/apps/`, `src/Makefile` | libnc runtime, demo apps, cross-build and deploy |
| `third_party/` | Khronos headers, SDL2 source + netcast patch + build script, prebuilt CMake, `sdl2-netcast/`, SDL2_mixer, Chocolate Doom, downloads in `dl/` |
| `build/package.sh`, `dist/` | NetCast+ package builder and output (`netcast-plus.zip`) |
| `build/` | BusyBox source and config, staged rootfs, `build-image.sh`, `netcast-root.ext4` |
| `usb/netcast/` | files deployed to the USB stick (image, BusyBox, `enter.sh`, `teardown.sh`, MD5SUMS) |
| `toolchain/` | buildroot-nc4 SDK |
| `tools/` | `tv.py`, `pull.sh`, `push.sh`, `netwatch.sh` |
| `src/kmod/` | out-of-tree kernel modules (nchello, xpad) for the stock 3.0.13 kernel |
| `src/qml/hello/` | QML Hello World (static Qt 6) |
| `third_party/kernel/` | linux-3.0.13 (configured for vermagic) + Bootlin GCC 7.3 toolchain |
| `third_party/qt/` | Qt 6.12 sources, `build-host.sh`, `build-target.sh`, eglfs_netcast plugin (in the qtbase tree) |
| `third_party/freerdp-build/`, `lvgl/`, `libvterm-0.3.3/`, `retro/` | FreeRDP, LVGL, libvterm, RetroArch + cores |
| `tv_dump/lglib/` | TV libraries and binaries pulled for reverse engineering, plus disassembly (`*.S`) |
| `tv_dump/glibc/` | LG glibc 2.12.2 from the TV's `/lib` |
| `sdk_extract/` | carved SDK resource zip (deletable) |
