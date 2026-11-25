# LG NetCast TV hacking: findings

Last updated: 2026-11-20. `telnet <TV IP>` gives a root BusyBox shell (no login). The host tools read the IP from `$NETCAST_TV` or `tools/.tv-host`.

**Status:** a native C program built here draws GPU-accelerated graphics on the TV screen
(EGL + OpenGL ES 2, 1280x720 at 57 fps), above LG's UI. Confirmed visually.

## Ground rules

- **Never write to the TV's internal eMMC** (a modified system partition can leave the TV permanently unbootable). **Always upload to the TV through the temporary busybox ftpd** (`tools/push.sh`). Only the USB stick
  (`/mnt/usb/usb2/Drive1`, vfat, exec allowed) and tmpfs `/tmp` (354 MB RAM) are used.
- Indirect flash writers to avoid:
  - `gst-launch` (`GST_REGISTRY=/mnt/flash/data/gst_registry.arm.bin`)
  - `gdb` (`GDB_ROOT=/mnt/lg/cmn_data`)
  - LG crash framework dumps (`/mnt/lg/cmn_data/*_pid*.txt`, `exc_log_*`)
  - app store installs (`/mnt/appstore`)
  - `/etc/checker.sh` (an MTK firmware installer, don't trigger it)
- Without `ncnetkeep` (start.sh runs it) the TV drops off the network for ~5 min every ~20 min (see "Wi-Fi drops").
  Check `uptime` before assuming a reboot.

## Hardware and OS

| | |
|---|---|
| SoC | MediaTek MT5398, 2x Cortex-A9 @ ~1 GHz, NEON, VFPv3 (no VFPv4) |
| RAM | 726 MB |
| GPU | PowerVR SGX 544MP (`pvrsrvkm`, DDK `1.9@2291151 cobra_lg_linux`), `/dev/mali` is a stale node |
| Kernel | Linux 3.0.13 (gcc 4.5.1), built 2017-05-20, `panic=1` |
| libc | glibc 2.12.2, ARM EABI5 **soft-float** (`ld-linux.so.3`). Build with `-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=softfp` |
| Root fs | squashfs, read-only, many squashfs/unionfs mounts under `/mnt` |
| Writable flash | ext4 partitions: `/mnt/flash/data`, `/mnt/lg/cmn_data`, `/mnt/appstore`, `/home`, `/mnt/system_app` (don't touch) |
| Load avg ~40 | harmless: MTK kernel threads in D state, CPU ~80% idle |
| kexec | not compiled in (only the weak `sys_kexec_load` stub). `nckexec.ko` does it from a module (see Kernel modules) |
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
  - Graphics calls go through `/dev/mtal` (MediaTek MTAL, `/dev/gfx` is a symlink to it) and `/dev/pvrsrvkm`.
    Nothing opens `/dev/fb*` or `/dev/fusion*`.
  - EGL/GLES live in `/mnt/lg/res/lglib`. The window system module is `libpvrPVR2D_CobraNetCast40WSEGL.so`.
  - **NetCast 4.0 mode (the default):** windows come from LG's HOA GX API in `libafopenapi.so`.
    `USE_NETCAST40=0` switches it to legacy `/dev/fb/N` mode instead.
  - DirectFB config is `/etc/directfbrc`: system=devmem, 1280x720 ARGB, sawman WM.
  - `libgal.so` and `libmtal.so` in `/mnt/vendor` are MediaTek's graphics/AV layers. Apps `LD_PRELOAD` them.
- App environment (`/proc/<fxui pid>/environ`):
  - `LD_LIBRARY_PATH` covers `/mnt/lg/res/lglib`, `/mnt/vendor`, `/usr/local/lib`, `/mnt/addon/lib` and more.
  - `LD_PRELOAD=/lg/res/lglib/liblibm.so:/mnt/vendor/libgal.so:/mnt/vendor/libmtal.so`.
  - Crash hooks: `HOOK_SEGV=YES`, `CRASH_FRAMEWORK=YES`.
  - `PM_EXECPID`, `PM_SHELLPID`, `PM_EXECPATH`, `PM_EXECARGS`.

## Native GUI recipe (working: `src/nctest/`)

Reverse-engineered from `libafopenapi.so`, `gxtest` and the WSEGL module. LG ships no headers.

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
5. `HOA_GX_GetWindowID(uint32_t *wid)` gets the window `procmgr` assigned (we got 26, the home UI was 27).
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
- SDK sysroot extras: Python 3.14 (has `ctypes`, no `ssl`/`zlib`/`sqlite3`/`curses`/xml modules),
  SDL2 2.32 (dummy video driver only), curl 8.22 with OpenSSL 3.6, sqlite 3.53, GStreamer 1.24.

Chroot on the USB stick (`/mnt/usb/usb2/Drive1/netcast/`):
- `enter.sh ['cmd']` loop-mounts `netcast-root.ext4` (1 GB, ext4 with only features kernel 3.0 supports)
  at `/tmp/ncroot`.
  - It binds `/dev`, `/dev/pts` and `/tmp`, mounts `proc`, `sysfs` and a tmpfs on `/run`.
  - Read-only TV dirs are bound at their original paths: `/mnt/lg/res/lglib`, `/mnt/vendor`,
    `/mnt/addon`, `/mnt/browser`, `/usr/local`, plus the D-Bus socket directory.
  - No TV flash partition is mapped in.
- `teardown.sh` unmounts the chroot.
- Rebuild on the host with `build/build-image.sh`. Output goes to `usb/netcast/`.
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
  `lginput` injects them via `/dev/uinput`. `procmgr` reads `event0..5`.
- Other nodes: event0 "LGE KEY RETURNPATH", event1 "LGE CURSOR RETURNPATH" (pointer), event3 "LGE NLP",
  event4 "Smart Remote RCU Input", event5 "LGE Smart Remote - TouchPad", event6 "LG Magic gamepad" (js0).
  USB keyboards/mice appear as new `eventN` nodes. libnc rescans every 2 s.
- **The HOA key/mouse callbacks never fire for our app.** `procmgr` doesn't route input to a self-launched
  service, so read evdev directly. Reading evdev doesn't grab the device, so LG's UI behind us may react to
  the same presses. Not solved yet: becoming the "focused app", or `EVIOCGRAB`.
- `HOA_PROC_SetReady()` takes no arguments. Calling it coincided with a crash, so libnc only calls it if
  `NC_SET_READY` is set. The HOA message callback's argument isn't a safe pointer: never dereference it.
- `HOA_PROC_UnregisterService()` returns -1 (harmless).

## Display resolution (verified 2026-11-19)

- **The panel is 1920x1080, but the graphics plane is 1280x720**, upscaled 1.5x in hardware (LG's UI too).
  `HOA_GX_GetWindowMaxResolution` = 1280x720, `DispRect` = 1920x1080, `fb0` = 1280x720 32 bpp double-buffered
  (`smem` 14.7 MB). `HOA_GX_SetWindowResolution(wid, 1920, 1080)` returns -1. Target 1280x720.
  (That limit is LG's API, not the hardware: the mainline kernel drives plane 3 at native 1920x1080, see
  "LG GPL source", OSD.)
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
  `SDL_Renderer` picks `opengles2`. `sdldemo` runs at 56.5 fps.
- Input: evdev keys go through `SDL_GetScancodeFromTable(SDL_SCANCODE_TABLE_LINUX, code)`. Text input is
  US layout. REL_X/Y become relative mouse motion, REL_WHEEL becomes the wheel, BTN_* become mouse buttons.
  `SIGTERM` becomes `SDL_QUIT`.
- `SDL_Log` is routed through `nc_log` (dbgfrwk swallows stderr). `NC_TIMEOUT=<s>` sets a hard kill timer.
- No audio yet (ALSA/Pulse disabled, only OSS/dummy). No SDL_ttf/SDL_image in the static build yet.
- Host CMake comes from Kitware's prebuilt tarball in `third_party/` (no system cmake, no python venv).

## Audio (working)

- No ALSA playback (only capture nodes `pcmC0D0c`/`pcmC1D0c`). PCM goes through MediaTek's GStreamer 0.10 element
  **`mtkaudiosink`** (in `libgstmtkelements.so`): `render-mode=1` = real output, 0 = fake (silent, default),
  2 = "dump" (don't use). Accepts `audio/x-raw-int` at 6–192 kHz.
- SDL audio driver `netcast` (`src/audio/netcast/SDL_netcastaudio.c`): dlopens GStreamer 0.10 and runs
  `appsrc ! mtkaudiosink render-mode=1`, paced about 100 ms ahead of the wall clock.
- **Flash hazard:** LG's env has `GST_REGISTRY=/mnt/flash/data/gst_registry.arm.bin`. `run-app.sh` copies it to
  `/tmp/ncapp-gstreg.bin` and sets `GST_REGISTRY_UPDATE=no`. The driver also refuses non-`/tmp` registries.
  For manual gst tools: `GST_REGISTRY=/tmp/x.bin`, and BusyBox `timeout -t N`. There's no `audioconvert` plugin.
- Seen once: a segfault during audio/OPL init in Doom, not reproduced in later runs. Not yet investigated.

## Display alpha and SDL window gotchas

- The TV composites our plane over video and LG's UI using **per-pixel framebuffer alpha**. Partial alpha shows
  the HDMI picture through. Alpha 0 (Doom's palette conversion) makes the window invisible.
  - SDL driver: forces alpha = 1 before each swap (`NC_ALPHA_OVERLAY=1` opts out).
  - ncgfx: uses `glBlendFuncSeparate(..., GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`.
- `SDL_CreateRenderer` recreates non-GL windows (Hide + Create). The driver re-shows the HOA window in
  `CreateWindow`. This was Doom's "sound but no picture" bug.
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
- Run: `sh /mnt/usb/usb2/Drive1/netcast/apps/doom/doom.sh`. Back is Escape (see Remote keys).

## ncplus package (launcher + apps)

- `build/package.sh` builds `dist/netcast/` and **`dist/ncplus.zip`** (about 12 MB, FAT-friendly).
  Unzip to the root of a FAT32 stick, then run `sh /mnt/usb/usb2/Drive1/netcast/start.sh` from telnet.
  It detaches with nohup and logs to `/tmp/netcast.log`.
- `start.sh` loops: `system/launcher` (tiles from `apps/*/app.ini`) -> the chosen app's `exec=` script -> launcher.
  Only one process owns the HOA window at a time.
- `app.ini` keys: `title=`, `desc=`, `exec=`, `color=` (hex RGB), `order=`.
- Still needed: autostart without telnet (needs a persistent hook = flash write, excluded), Magic Remote
  pointer, more apps.

## More apps / screen control (2026-11-20)

- **OpenTyrian** v2.1.20260913: plain Makefile, built with `SDL_CPPFLAGS/SDL_LDLIBS` overrides against the static
  SDL2. Data is `tyrian21.zip` from camanis.net (freeware release, but the zip still contains the 1995 EULA).
  Bundled only with `build/package.sh --with-tyrian-data`. Preset `opentyrian.cfg`: fire=Return,
  change fire=F1, sidekicks=F2/F3, Scale2x, 4:3.
- Back is Escape and CH- is PageDown in the SDL driver. Doom's cfg uses 27 for menu, back and abort.
- Screen off with Wi-Fi kept: there's no direct HOA setter (`HOA_CTRL_IsScreenoff` only). LG's
  *Picture -> Energy Saving -> Screen Off* does it. Candidates for a tool: `HOA_CTRL_SetPicControl` (enum values
  unknown, could change saved picture settings), and MTAL `MTPMX_PANEL_EnableLVDSPower` /
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
  - `src/apps/ncscreen.c`: `on|off`, `off --wake`, and `saver [min]` (idle daemon). Verified on screen.
    Wi-Fi stays up. LG's own Picture->Energy Saving->Screen Off seemed to take the network down.
  - The ack read returns 0xff from our process, but the command works.
- **Sleep timers:** RELEASE keeps settings in a RAM shadow at `0x03148440`. Auto Off is `+0x936D` (15-min timer
  `0x4000000B`), No-signal power off `+0x937E`, No-operation standby `+0x9380`. Menu setters write the byte and then call
  `UI_SVC_REGISTRY_WriteNVRAM` (flash). `src/tools/ncnosleep.c` pokes the bytes to 0 via ptrace (RAM only) after
  verifying firmware code words. On this TV, Auto Off and No-signal were ON: the cause of the "TV went to sleep" episodes.
- `/proc/<pid>/mem` is not readable on 3.0 without ptrace attach. `src/tools/peek.c` reads words via ptrace.
- RELEASE (19 MB) is not stripped: its symbol table makes RE practical. A copy is in `tv_dump/release/`.
- **Services:** `HOA_PROC_DisableAndStopServices(const char **names, int n)` overflows its own heap buffer for long
  lists, so call it once per name (`src/apps/nclite.c`). LG's `svclist_gip.rc` itself disables myappsmini, voicenlp
  and webbrowser. nclite still needs a live test.
- Every LG app maps `/mnt/lg/cmn_data/appfrwk_mask` (flash) MAP_SHARED rw. run-app.sh points `APPFRWK_MASK_PATH` at `/tmp`.
- One crash log of ours exists on flash: `/mnt/flash/data/exc_log_nctest_pid2379.txt` (1.9 KB), from the first
  nctest crash before our handlers were reinstalled after each dlopen. Left in place: deleting it would also write flash.

## Media player, hardware decode (2026-11-20)

- GStreamer 0.10 `playbin2` with `video-sink=mtkvideosink` and `audio-sink=mtkaudiosink render-mode=1` plays through
  the MediaTek decoders. LG's patched playbin hands H.264 straight to the sink, which decodes it on the video plane.
  The OSD is cleared to alpha 0 so the video shows through, with an info bar drawn on top (`src/apps/ncplay.c`).
- Elements present: `omx_h264dec`, `omx_mpeg2dec`, `omx_mpeg4dec`, `omx_aacdec`, `omx_mp3dec`, `omx_ac3dec`,
  qtdemux, matroskademux, mpegtsdemux, flv, souphttpsrc, rtspsrc, udpsrc. **Not working:** raw AAC in MP4
  ("Missing element: MPEG-4 AAC decoder", but ADTS in TS is fine) and HLS (no hlsdemux).
- The TV has no internet route (gateway unreachable, DNS proxy fails). For tests, serve media on the TV itself:
  chroot `python3 -m http.server 18080 --bind 127.0.0.1` (LG uses 8080).
- Bus messages are polled with `MSG_TYPE(m) = *(int*)((char*)m + 24)` (GStreamer 0.10 GstMessage layout).
- VLC/mplayer would decode in software on 2x A9: fine for audio and SD, not for HD H.264. The hardware path is the
  one to use.

## ffplay, Web Browser (NetSurf) and the test folder (2026-11-20)

- **ffplay** (FFmpeg 9.1, `build-deps.sh ffmpeg`): static except glibc, NEON software decode, GLES2 output through
  SDL. It handles what the hardware Player can't (VP9, HEVC, Opus, AAC in MP4). It has no launcher tile:
  Network Shares opens a video with YELLOW (`apps/ffplay/run.sh <file|URL>`), and OK still opens the hardware Player.
- **NetSurf 3.11** (`build-deps.sh netsurf`, framebuffer frontend on our SDL2 libnsfb surface). Build gotchas:
  the browser's own Makefile ignores `HOST` and only takes `CC`. Without it the framebuffer frontend falls back to
  the host `cc` and links x86 objects. `NETSURF_CONFIG="CC=… CXX=…"` passes them to that sub-make only. NetSurf
  adds plain `-lz -lexpat`, and the TV has zlib 1.2.3 (no `ZLIB_1.2.3.4` symbols) and no expat, so the static
  archives are linked in from a `-L` dir that holds no `.so`. Result: needs only glibc ≤ 2.10.
- **NetSurf runtime:** `--option=value` arguments are only parsed *before* all others (`-w`, `-h`, URL).
  `NETSURFRES` = `apps/netsurf/res` (Messages, CSS, DejaVu fonts, `Choices`). Toolbar images are compiled in.
  HTTPS needs `--ca_bundle` (the TV has no CA store, the package ships Debian's bundle). The "undefined
  instruction" lines in dmesg at startup are OpenSSL probing CPU features (SIGILL is caught), not a crash.
  Exit: a 5 s watchdog plus `_exit()` after teardown, like ffplay, because LG's destructors can hang.
- **No internet on the TV**, and DNS is broken too (the 127.0.0.1 resolver isn't listening, at least after the
  warm boot). `tools/webtest.sh` runs the static BusyBox `httpd` on 127.0.0.1:8090 over `netcast/test/`
  (browser test pages, synthetic media from `build/make-testmedia.sh`, test photos with a static index, since
  BusyBox httpd has no directory listing). It writes `/tmp/webtest.conf` with MIME types. Without it, `.mp4` and
  `.mkv` are served as `text/html`. Range requests work (206). Player, Photos and the Web Browser start it when
  netcast.conf (or a URL argument) points at 127.0.0.1:8090.
- **Hardware decode, measured on the TV** (`gst-launch-0.10 playbin2` with `mtkvideosink`/`mtkaudiosink`,
  and ncplay): **H.264 (Main or High) + MP3 in MPEG-TS plays cleanly.** H.264 + AAC in TS plays video only
  ("Missing element: MPEG-4 AAC decoder"). MPEG-2 video: "Missing element: MPEG-2 Video decoder". H.264 + AC-3
  in TS and MPEG-2 + AC-3: data-flow/not-negotiated errors. **Any MP4, even H.264 with no audio, fails in
  qtdemux**. This contradicts the element list above (omx_mpeg2dec, omx_aacdec, omx_ac3dec exist): they don't
  link in this setup. The hardware test clip is therefore H.264 + MP3 TS. Other formats go to ffplay.
- **ffplay, measured:** VP9 + Opus 360p30 in sync (A-V within ±0.02 s), 33/450 frames dropped. HEVC 360p30 in
  sync but 238/450 dropped (too slow, so no HEVC test clip). Music plays FLAC/MP3/WAV via mtkaudiosink 44.1 kHz.
- **procmgr slots leak (the cause of "RegisterService failed"):** `HOA_PROC_UnregisterService` sends one bus
  request with our pid, and procmgr never answers it (rc=-1, even as the first teardown step, while other procmgr
  queries on the same connection succeed). `HOA_PROC_GetRunningProcessCount` grows by one per app run and
  never drops (32, 34, 35, ..., and `NotifyAppTermination` doesn't help). After roughly a dozen launches (each
  return to the launcher counts) registration fails for every name and ncplus exits. Only a power cycle
  clears it. libnc has `NC_PM_DEBUG=1` (logs the count) and `NC_PM_NOTIFY=1` (experiment) for this. Not fixed.
- Tested on the TV (cold boot, user watching): the Web Browser renders the test pages (CSS, PNG, links,
  scrolling, Back) and ffplay plays the software test clips. The user signed both off.

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
  if present, otherwise `/bin/sh -l`. USB keyboard with US layout. The remote's CH- sends ^C and CH+ sends Tab.
  `TERM=xterm-256color`.

## Remote Desktop: FreeRDP 3 (2026-11-20)

- FreeRDP 3.33.1, SDL2 client, static except glibc (9.6 MB stripped). Notes for the cross build
  (`third_party/freerdp-build`):
  - The SDL2 client needs **SDL2_ttf** for its dialogs. It's built static (2.24.0, system FreeType, no HarfBuzz)
    into `sdl2-netcast/`.
  - glibc 2.12 has no `<threads.h>`: build with `-D__STDC_NO_THREADS__`, so `WINPR_TLS` becomes `__thread`.
  - LTO is on by default and needs `CMAKE_CXX_COMPILER_AR`, which the SDK lacks: `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF`.
  - Pass `FREETYPE_LIBRARY` as a single path. bz2 and zlib go in `CMAKE_CXX_STANDARD_LIBRARIES`.
  - `-latomic` (from the OpenSSL static deps) resolves to a `libatomic.so` that is `libatomic.a`, placed
    first on `-L`: the TV has no libatomic.so.1.
  - Options: OpenSSL 3.6.4 static from the SDK, without X11, FFmpeg, ALSA/Pulse, CUPS, PCSC, Kerberos, FUSE, JSON or AAD.
- The netcast SDL driver sends real scancodes, which the FreeRDP SDL client needs for keyboard input.
- `apps/rdp/rdp.conf`: one line of arguments (`/v:HOST /u:USER`). run.sh adds
  `/size:1280x720 /f /cert:tofu /gfx /sound:sys:fake +clipboard`. The password prompt is FreeRDP's SDL dialog.
- Not tested on the TV yet. Open questions: the mouse cursor (our driver has no hardware cursor) and audio (no
  SDL rdpsnd backend, and sound is disabled with `fake`).

## Kernel modules (2026-11-20)

- **Build (since 2026-11-23):** LG's own MT5398 kernel source from their NetCast 4.0 GPL drop (see "LG GPL
  source" below), `mt5398_smp_mod_defconfig`, with the Bootlin armv7-eabihf 2018.11 toolchain (GCC 7.3,
  `compiler-gcc7.h` copied from gcc4). `third_party/build-deps.sh kmod` unpacks and prepares it from the zip. Then run
  `make -C src/kmod`. vermagic matches: `3.0.13 SMP preempt mod_unload ARMv7`. No MODVERSIONS (LG's modules
  have no `__crc_`). Until then the build used vanilla 3.0.13 + vexpress_defconfig, which got `input_dev`, HZ
  (100 vs LG's 200), HIGHMEM and PHYS_OFFSET wrong.
- **`struct module` matches** LG's (`tools/kmod-check.sh` against `tfat.ko`: 0x140 bytes, init +0xbc, exit +0x138).
  `nchello.ko` loads and unloads cleanly from `/tmp`.
- **LG's `struct input_dev` is 136 bytes larger** between `keybit` and `open`: `open`/`close` at 500/504 and `dev`
  at 560 (vanilla: 364/368/424). `name`, `phys`, `id`, `evbit`, `keybit`, `struct urb` and `usb_device` up to
  `descriptor` match. Modules must not touch `input_dev` fields past `keybit`. Found by disassembling LG's
  built-in `xpad_probe`, read from `/dev/mem` (phys = virt − 0xc0000000). The `struct module` check does not catch this.
  **Cause (LG source):** LG raised `KEY_MAX` from 0x2ff to 0x500, which grows `keybit[]` and `key[]` by 17 longs
  each. Modules built against LG's tree get the right offsets (`open` 500, `dev` 560, sizeof 776).
- **Built into LG's kernel:** xpad (original Xbox and Xbox 360 pads, rumble, no Xbox One), usbhid, hid-sony.
  A PS3 pad (`054c:0268`) works as is. More 360-protocol pads: `echo VID PID > /sys/bus/usb/drivers/xpad/new_id`.
- **`xpadone.ko`** (`src/kmod/xpadone.c`): Xbox One/Series wired pads (GIP, interface `ff/47/d0`), touching only
  the matching fields. These pads also have device class `0xff`, so the ID table matches vendor + interface
  (on 3.0 a bare `USB_INTERFACE_INFO` never matches). Other vendors:
  `echo VID PID ff > /sys/bus/usb/drivers/xpadone/new_id`. Tested with a PowerA `20d6:2001`: all buttons, sticks
  (±32768), triggers (0–1023), d-pad (hat), rmmod/insmod. Guide hasn't been seen yet. `start.sh` loads it only on
  the kernel build it was checked against.
- `src/kmod/xpad.c` (vanilla xpad) is not shipped: it duplicates the built-in driver. Built against LG's tree it
  no longer writes `input_dev` at vanilla offsets, but it is untested.

### Warm boot of another kernel from RAM: `nckexec.ko` (working, 2026-11-20)

> **Never boot a different (non-stock) kernel on a TV that doesn't pass `ncpreflight`.** A kernel that misbehaves
> can leave it permanently unbootable. `nckexec-fire.sh` refuses.

`src/kmod/nckexec.c` hands the running kernel over to another kernel in RAM. No flash write is involved.
**Tested on this TV:** booting the stock kernel through it gives a working system. It was back on telnet
~70 s after the jump, LG's RELEASE was running, all 726 MB of RAM was present, and the remotes reconnected.
Proof that it came from our handoff and not a reset through U-Boot: `/proc/cmdline` ends in the added
`maxcpus=1 panic=5`, only one CPU is online, and printk timestamps start at ~1079 s while uptime is ~74 s
(the SoC timer was never reset).

- **Kernel image:** p21 is an MTK LZ4P container (`tools/unlz4p.py`): a header, the u32 compressed size of each
  64 KiB block, then raw LZ4 blocks. It holds a uImage (load/entry `0x7fc0`, U-Boot `bootm 0x7FC0`) around a raw
  ARM `Image` linked at phys `0x8000` (`stext` 0xc0008000, PHYS_OFFSET 0). It matches the running kernel's text
  byte for byte. machine_desc: nr 5398 `MT5398`, `boot_params` 0x100, `fixup` NULL. The tag table has only the
  standard ATAGs.
- **Staging (`insmod`):** the image goes into single pages, all above 16 MB (so none lies in the destination,
  no large contiguous allocation). ATAGs are built fresh: CORE, MEM per `System RAM` bank
  (`0x26000000@0`, `0x08800000@0x40000000`), and CMDLINE (this boot's line plus `append=`, and busybox insmod
  drops quotes, so `append=` takes `;` between arguments). A {dest, src, len} list puts the ATAGs at `0x100` and
  the Image at `0x8000`.
- **`go`:** first `kernel_restart_prepare()`, which runs the reboot notifiers and `device_shutdown()` (stops USB
  DMA). Then IRQ/FIQ off, `setup_mm_for_reboot()` (1:1 map), and a flush of L1 and the PL310. A stub at its
  physical address, using no stack and no kernel code, then does these steps:
  1. D-cache off.
  2. L1 clean+invalidate by set/way.
  3. PL310 clean+invalidate by way.
  4. MMU, I-cache and branch prediction off.
  5. Copy.
  6. Enter with `r0=0, r1=5398, r2=0x100`.
- **PL310:** r3p2 (`0x410000c8`), 16-way, already enabled. The kernel maps it at VA `0xf2000000`. `l2x0_base` is
  not in kallsyms. Its address comes from the literal pool of `l2x0_disable` (`ldr r5, =&l2x0_base`), and
  `l2x0_way_mask` follows it. `__l2x0_flush_all` is a plain way operation with no debug-register writes.
- **CPU1:** without a hold request it has to be offlined first (`echo 0 > /sys/devices/system/cpu/cpu1/online`).
  The stock `platform_cpu_die` flushes L1, clears SCTLR.C (its ACTLR `bic #0x20` is ARM11MPCore's SMP bit, and on the
  A9 that bit is reserved), and waits in WFI for `pen_release`. The next kernel must use `maxcpus=1`: releasing
  CPU1 would send it back into the old kernel's hotplug code. With a DTB hold request nckexec parks CPU1 in a
  holding pen of its own instead (see "Secondary CPU boot" below).
- Module gotchas on this tree: no `%pa` (it arrived in 3.8). With the old vanilla/vexpress build,
  `PLAT_PHYS_OFFSET` had to be patched to 0, otherwise `virt_to_phys` was off by `0x60000000`. LG's tree has
  PHYS_OFFSET 0 natively.
- Run: `src/kmod/nckexec-fire.sh`, copied to the stick next to the module and the image (`sync`, CPU1 offline,
  `insmod … 'append=maxcpus=1;panic=5'`, `go`).
  If the new kernel fails it panics, and `panic=5` reboots into LG's system.

## Qt 6 / QML (in progress, 2026-11-20)

- Qt 6.12.0 (qtbase, qtshadertools, qtdeclarative). Host tools are built in `third_party/qt/host`
  (`build-host.sh`). The target is a **static** build (`build-target.sh`): eglfs, GLES2 against LG's EGL/GLES
  (stubs + `--allow-shlib-undefined`), evdev input, bundled FreeType/HarfBuzz/PCRE/zlib/libpng, without fontconfig, udev,
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
  percent-escapes decoded) and connect/share enumeration (`smb2_share_enum_async` on IPC$, hidden, `$` and
  non-disk shares filtered).
- **`smbgw`** (`src/apps/smbgw.c`): read-only HTTP gateway on 127.0.0.1:8445.
  - Routes: `/s/<entry>/<share>/<path>` and `/l/<path under /mnt/usb>`.
  - Directories come back as an HTML link index, the format Photos already parses. Files support HEAD and Range.
  - One SMB connection per share, mutex-shared, reconnected once on error. `..` is refused, and shares.conf is
    reloaded when it changes.
  - This is how Photos, the hardware Player (souphttpsrc) and Music read SMB files without any SMB code.
- **Network Shares** (`ncsmb.c`, SDL2 + SDL2_ttf via `sdlui.c`): browses the shares and the USB stick, and hands
  files to the other apps. Music gets an m3u, Photos gets a conf with `url=` the gateway folder, and the Player gets
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
  - `NCMUSIC_SELFTEST=1 ncmusic <items>` decodes the items headlessly (format, tags, cover, full decode, seek).
- **Tested:** on the host against impacket's SMB2 server (auth, share list, listings, ranges, 4 parallel streams
  byte-exact, traversal 403, missing file 404). On the TV: ARM decode >10x real time, gateway local route,
  Music UI with audio (user heard it), and the browser -> Music -> browser hand-off. Not yet tested on the TV
  against a real SMB server.
- Test-only: `NC_KEYS="Down,Return"` makes sdlui apps press keys (one every 0.7 s), and it's not passed on to
  apps they start.
- Gotcha: the TV's BusyBox has no md5sum or wget. Use `netcast/busybox`. Also, `pkill -f <name>` from a shell
  whose own command line contains <name> kills that shell (use `pkill -x` or a `[n]ame` pattern).
- Host disk: the Qt Quick host build alone used ~9 GB (PCH). Rebuild with `-DBUILD_WITH_PCH=OFF` and fewer modules.

## Remote keys: Back and volume (2026-11-20)

- Input devices: event0 "LGE KEY RETURNPATH", event1 "LGE CURSOR RETURNPATH", event2 "LGE RCU", event3 "LGE NLP",
  event4 "Smart Remote RCU Input", event5 "LGE Smart Remote - TouchPad", event6 "LG Magic gamepad".
- Captured with `src/tools/evdump.c` (read-only). All on event2: **Back = 412 (KEY_PREVIOUS)**, Vol+ 115,
  Vol- 114, Mute 113, OK 28.
- libnc maps 412 to KEY_ESC for every app. The SDL driver maps Escape, so Back is back/menu everywhere, and CH-
  is now PageDown. Photos and Player no longer treat CH- as back. The terminal keeps CH- = Ctrl-C.
- **Volume:** while our app has focus, LG doesn't act on volume keys. libnc calls LG's API from libbcopenapi
  (loaded lazily), with the calling convention taken from RELEASE's own caller `UI_GESTURE_CtrlSetVoluem`:
  `HOA_CTRL_SetVolume(0 /*relative*/, 2 /*type*/, 1, ±1, &newvol)`. The RS-232 handler uses
  `(1 /*absolute*/, 2, 0, level, NULL)`. `HOA_CTRL_GetCurrentVolume(2, &u8)`. Mute is
  `HOA_CTRL_GetMute(&m)` + `HOA_CTRL_SetMute(!m, 1)`. LG persists the level
  itself, as for any volume change. `NC_NO_VOLUME=1` disables this. Not yet tested on the TV.
  Qt apps (`no_evdev`) don't get this yet.
- RELEASE's key-name table at file offset ~0xdd6c14 is `{const char *name, uint32 linux_code}` pairs
  (IR_KEY_VOL_UP -> 115, IR_KEY_CH_UP -> 402, IR_KEY_EXIT -> 174): the IR-to-evdev conversion for "LGE RCU".
- `HOA_PROC_UnregisterKeysFromAppKeyTable(const uint32_t *keys, uint32_t n)` exists (it would hand keys back
  to LG). The key code space it uses is unknown, so it isn't used.

## Wi-Fi drops: no broadcast RX, router ARP ageing (fixed with `ncnetkeep`, 2026-11-21)

- **Symptom:** unreachable for ~5 min after every ~20 min up (logs/tv-reachability.log: up 20:00-20:09, down
  4:57-5:00, around the clock), while the AP still lists the TV as associated.
- **Cause, TV side:** the Broadcom fullmac firmware (`wl0` 6.30.100.15) drops every group-addressed frame. The
  radio receives them, but none reach Linux. Counters (`wlq cdiff`, `counters` v8):
  `rxdfrmmcast` (index 82) = 8164 after 28 min, while `rxmulti` (115) = 0, wlan0 `multicast` = 0, and pktlog
  shows bc=0 mc=0. Every software drop/decrypt counter is 0. AP: RSN group=CCMP pairwise=CCMP AKM PSK
  (`wlq bss`), so it isn't a TKIP group-cipher mismatch. `sup_auth_status` = 6 (keyed), and `allmulti` 1 has no
  effect. Unicast both ways and broadcast *transmit* work. Root cause inside the firmware: not found.
- **Cause, network side:** the TV can't hear ARP requests. The gateway (clients on another subnet
  are routed) re-checks its entry with a broadcast, gets no answer and drops it after 20 min (FreeBSD/pfSense
  `max_age` = 1200 s fits). Linux on the TV never ARPs the gateway while TCP keeps confirming it, so nothing
  recreates the entry until the next DHCP renew (lease 600 s, renew every 300 s) sends a unicast ARP probe.
  That is why the outage always lasts a little under 5 min.
- **Fix:** `src/tools/ncnetkeep.c` (static, tools/ in every build) sends, every 30 s: a gratuitous ARP, an ARP
  request for the default gateway (broadcast) and a unicast ARP request to each complete peer in
  /proc/net/arp. A host that gets an ARP request for its own address (re)creates its entry for the sender.
  start.sh starts it when `[settings] NETKEEP=1` (default). Settings has a "Wi-Fi keep-alive" switch. Verified
  2026-11-21: started at 10:40, no drop at the predicted 10:55 expiry or after (gateway replies to each round).
- Router-side alternative: a static ARP entry for the TV on the gateway (pfSense: DHCP static mapping with
  "ARP Table Static Entry").
- Tools: `wlq bss` (AP RSN/WPA IE), `wlq cdiff [s]` (counters that moved), `pktlog`, `package/tools/netmon.sh`,
  host `tools/netwatch.sh`.

## Flash storage (2026-11-20)

Read-only dumps: `tools/tv-dump-flash.sh`. They hold per-device keys and LG's code: never publish one.
This project never writes to flash and runs from the USB stick.

- eMMC only: `/dev/mmcblk0`, ~3.8 GB, 63 partitions (no NAND/MTD). System partitions are read-only squashfs
  (`root=/dev/mmcblk0p16 rootfstype=squashfs`).
- Writable ext4: p62 (`cmn_data`, `home`, `cache`, `user`, `flash/data`), p61 `system_app`, p63 `appstore`,
  p15 hdcp, p12 ciplus, p31 vcs.
- **PART.INFO** (p2, byte-identical backup `mapbak` = p3) maps logical names to physical extents. Header magic
  `16 07 12 20`, device `mtk3598-emmc`. 96-byte records: `+0x00` name, `+0x20` physical offset and size,
  `+0x30` filename, `+0x50` image size, `+0x54` version (`0x04031001` = v04.03.10), `+0x58` flags, `+0x5c` type.
  A CRC over the first 0x3050 bytes is stored at `+0x3050`. Per-partition CRC32s are in `CRC32.INFO`.
- **Banks:** updatable partitions (rootfs, kernel, lginit, swue, LG app/res) have two records. The active one
  has flags bit0 set (`0x101`, inactive `0x100`). U-Boot uses the first active record per name to build
  `root=`, `lginit=` etc. The inactive records hold the previous firmware, v04.03.09.

## LG GPL source (2026-11-23)

LG's GPL/LGPL drop for this platform, `NetCast4.0_M_CO_FI_20131031.zip` (opensource.lge.com, ~730 MB), kept
outside git (repo root / `third_party/lg-gpl/`, both ignored). Of interest: `GPL/kernel-m13.tgz` (MT5398
linux-3.0.13: a full tree plus LG's `-svn` overlay, copied over it), `GPL/packing_uboot.tgz` (U-Boot 1.3.4 with
board/mt5398 and LG's `cmd_lg`), `LGPL/directFB_nc4.tar.gz`, and `etc/gnuarm_4.5.1_cortex-a9_vfp` (MediaTek
GCC 4.5.1, i686 host binaries). The source is from 2013. The TV's kernel was built 2017-05-20 with the same GCC
4.5.1, so details can differ. The details checked against the live TV so far matched.

- **Module ABI:** see "Kernel modules". `KEY_MAX` 0x500, `CONFIG_HZ=200`, HIGHMEM, NR_CPUS 2, PHYS_OFFSET 0.
  All of `src/kmod` builds against it and `tools/kmod-check.sh` matches LG's `tfat.ko`, and the `nckexec.ko`
  built this way did mainline boots 5–7.
- **USB host (`mtk_hcd.c`, `mtk_cust.c`):** four MUSB cores at `0xf0050000 + n*0x1000`, PHYs at
  `0xf0059800 + n*0x100`, IRQs SPI 15/11/48/49, 5 TX + 7 RX endpoints each, 32-bit RMW register writes,
  L1 interrupts `0xa0`/`0xa4`, FIFO width `0x690`, toggles `0x80`/`0x84`: all as the mainline glue assumed.
  `MGC_PhyReset` turns TM1_EN (`0x604` bit 0, fast OTG timers) on only while the session opens and then clears
  it, and sets the slew rates in PHY `+0x10` (low byte `0x11`, bits 18:16 = 4). On the Wi-Fi port (3) it applies
  the OTG bits after `msleep(300)` so the built-in dongle re-enumerates. Stock's live registers on all four ports
  agree (PHY `+0x10` = `0x00045511`, `0x604` = `0x8`) and do *not* have Vizio's `0x60000` in PHY `+0x18`.
  The mainline glue left TM1_EN set and skipped the slew rates. It was fixed to match, after which mainline
  enumerated a USB stick (usb-storage, `sda1`), a PL2303, the Wi-Fi dongle and an AX88179B Ethernet adapter.
  VBUS (non-native-GPIO path): port 0 = TCON12 (`0xF000D728`/`D708` bit 21), port 1 = GPIO46 (`D724`/`D704`
  bit 14), port 3 = ADIN2 (`0xF00280BC`/`80`/`78`). Port 2 has no switch.
- **BIM interrupts:** banks IRQ/MISC/MISC2 at `0x30`/`0x48`/`0x80` (status, enable, clear), MISC and MISC2
  cascades = IRQ bank bits 30/31, as the mainline driver does. **Watchdog reset:** PDWNC `+0x104` = `0xff000000`,
  then `+0x100` = 1, as in the mainline DTS.
- **OSD scaler, native 1080p (works under mainline, 2026-11-23):** U-Boot `board/mt5398/module/osd_*.c`.
  Scalers SC1/SC2/SC3 at OSD base `0xf0021000` + `0x400`/`0x500`/`0x600` serve planes 1/2/3
  (`OSD_SCALER_TO_PLANE`). SC words: `+0` control (bit 2 V-up, 3 V-down, 4 H-up, 5 H-down, 7 enable),
  `+4` src (W<<16 | H), `+8` dst, `+0xc` vsc H size, `+0x10` H-down step/offset, `+0x14` H-up step/offset,
  `+0x18` V offsets, `+0x1c` V step (steps are 14-bit fixed point). Stock's SC3: control `0x94`, src
  1280x720, dst 1920x1080, H step `0x2aa7`, V step `0x2aa5` (the 1.5x upscale). SC1/SC2 already run 1:1 at 1080.
  Region header: word 1 bits 23:0 = data address bits 27:4, word 2 bits 25:24 = bits 29:28, word 7 bits 28:27 =
  bits 31:30. Word 4 = input width | height << 12 | (pitch / 16) << 23. Word 6 bits 10:0 = output height,
  word 7 bits 10:0 = output width. The OSD fetches the header from DRAM every frame (plane `+0x20` mirrors it),
  so editing it in place takes effect. Update strobe: OSD base `+0` bit 0, then bit 3 (`_OSD_UpdateReg`).
  For 1:1, LG's `OSD_SC_Scale()` clears the up/down bits and zeroes steps/offsets with src = dst. nckexec does
  this after the stock shutdown when the DTB carries `nckexec,osd-1080`: 1920x1080 ARGB, pitch 7680, buffer at
  the top 8 MB of bank 0 (`0x25800000`, reserved no-map). Confirmed on the panel.
- **SIF I2C (LG GPL U-Boot `board/mt5398/module/sif_hw_v2.c`, `sif_hw.h`, `LGE_sif_cfg.h`):** MediaTek's
  SIF buses 0 (system), 1 (tuner) and 2 (second) share master 0 at BIM `+0x21240` = `0xf0029240` (CTRL0, CTRL1
  `+4`, D0 `+0x10`, D1 `+0x14`, CTRL3 `+0x44`). Only the pad mux tells them apart, and LG sets it before every
  transfer: CKGEN `0xf000d400` bits 23:22/21:20 and `0xf000d404` bits 1:0 (bus 1 = osda2/oscl2: `d404[1:0]=1`,
  `d400[21:20]!=1`, `d400[23:22]!=2`). Buses 3 and 4 are the PDWNC masters (`0xf00281c0` / `0xf0028160`). CTRL0:
  CLK_DIV bits 27:16 (SCL = 27 MHz / div), deglitch 15:8 + enable bit 5, SCL/SDA state bits 3/2, enable bit 1.
  CTRL1: TRI bit 0 (busy while 1), mode bits 6:4 (1 start, 2 write, 3 stop, 4 read + NACK, 5 read + ACK),
  PGLEN bits 10:8 (bytes - 1, up to 8), ACK bits 23:16 (one per byte). This is the same command layout as
  MediaTek's later MT7621 I2C block. Mainline driver: `src/mainline/kernel/drivers/i2c/busses/i2c-mt5398-sif.c`.
- **Backlight under mainline (written 2026-11-24, on the TV the driver binds, but the one blank attempt, at
  1267 s, failed: `backlight off failed: -6`, ENXIO after 3 tries, cause unknown):** `lg-micom-bl` (I2C client 0x29 = LG's
  8-bit 0x52 on SIF bus 1, divider 380) sends `{0x37,0,0}` off / `{0x36,0,0}` on, waits 10 ms and reads the
  1-byte answer, which is only logged. It is `/sys/class/backlight/micom` (max 1, so `bl_power` 4 = off). The
  mt5398_osd CRTC disable/enable switches it, so DPMS off darkens the panel. Debian's Xorg now sets DPMS
  off after 20 idle minutes (`xset dpms force off` does it at once). On shutdown the driver turns the
  backlight back on.
- **TrustZone:** `CC_TRUSTZONE_SUPPORT` keeps the last 16 MB of the RAM hole for the secure world:
  `0x3f000000`–`0x3fffffff`, which is why reads there bus-error from Linux.
- **Secondary CPU boot:** the entry point's physical address goes in BIM `+0x188` (TIMER1_HLMT), magic
  `0xffffb007` in BIM `+0x68` (TIMER1_LLMT), then `sev`. Stock hotplug parks CPU1 in a WFI loop inside the stock
  kernel's own text (MMU on, D-cache off), which the next kernel overwrites at `0x8000`.
- **CPU1 under mainline (written 2026-11-24, works on the TV: 2 CPUs up at boot):** the DTB's `nckexec,cpu1-hold = <0x0cc1b007
  0x3e100000>` makes nckexec keep CPU1 online and, after `kernel_restart_prepare()`, run a kthread bound to CPU1
  that disables IRQs, drops CPU1 from the online mask (as `ipi_cpu_stop` does), installs the 1:1 map, flushes L1
  and enters a 320-byte stub copied to the hold page `0x3e100000` (RAM hole, the untouched 8 MB that ramoops is
  in). The stub turns the D-cache, MMU, I-cache and branch prediction off, stops CPU1's A9 private timer, disables
  its GIC CPU interface, stores ACTLR/CPSR at `+0xff8/+0xffc`, writes `0x5398c1a0` (waiting) at `+0xff0` and
  waits in WFE for a non-zero entry at `+0xff4`. ACTLR is left as it is (the kernel may be non-secure).
  Mainline `arch/arm/mach-mt5398/platsmp.c` (`enable-method = "netcast,mt5398-smp"`) releases CPU1 only if the
  page says waiting (`__pa(secondary_startup_arm)` -> `+0xff4`, `sev`). Otherwise it marks CPU1 not present,
  and it never sends CPU1 an interrupt. The BIM mailbox is not used, so stock's leftover TIMER1 values don't
  matter.
- **LG 2D GPU API:** DirectFB's `dtv_lib/ddi_gpu.h` + `ddi_gpu_data.h` (LGPL) declare `DDI_GPU_*` (layers, flip,
  blit, stretch blit, fills, hardware image decode), loaded from `librpc4dtv.so.1`, which the TV has in
  `/usr/local/lib`. Unused so far. It is a candidate for accelerated SDL blits and the Photos app.

## Mainline display (KMS) and the SGX544 (2026-11-23, works on the TV)

- **The SGX doesn't scan out.** The panel is fed by MediaTek's OSD (plane 3). The GPU only
  renders into memory, so "a driver instead of simplefb" means a KMS driver for the OSD.
- **`mt5398_osd`** (`src/mainline/kernel/drivers/gpu/drm/tiny/mt5398_osd.c`, patch 0003,
  `CONFIG_DRM_MT5398_OSD`). One CRTC, one primary plane (XRGB8888 and ARGB8888, linear),
  an LVDS connector with the fixed 1080p60 mode, GEM DMA buffers from a 64 MB CMA pool at
  `0x10000000`-`0x1fffffff` (DTS `osd-pool`). It takes over only the native 1080p state that
  nckexec leaves (header w0 ARGB8888 without a chained region, w4 `0xf0438780`), and refuses
  other states, in which case simplefb stays. Then it unregisters simplefb (aperture), and
  fbcon moves to its fbdev (ARGB8888, so the palette is opaque, as on simplefb).
  A flip rewrites header w1 (address bits 27:4 plus mix weight) in place, then strobes OSD
  `+0` bits 0 and 3. LG's U-Boot region layout (`OSD_RGN_FIELD_T`): w1 bits 31:24 = mix weight,
  w7 bits 31:30 = mix select (0 none, 1 pixel, 2 region, 3 plane). U-Boot's logo uses region
  with weight 0xff. ARGB8888 keeps stock's weight and select. XRGB8888 uses region + 0xff
  (module parameter `mt5398_osd.xrgb_mix`, untested). The plane enable bit (`+0x300` bit 0)
  follows the CRTC (DPMS). No OSD vsync interrupt is known, so vblank is a 60 Hz hrtimer.
  A flip's event waits until the fetched-header mirror (`+0x324`) shows the new w1, for at
  most 3 ticks. The probe logs the header, the mirror and SC3.
- **SGX544MP1** facts (from LG's `pvrsrvkm.ko` and the GPL DDK headers in `powervr_sgx/kernel`):
  [docs/sgx544.md](docs/sgx544.md). Window `0xf0040000` + `0xc000`, GIC 85 (BIM MISC 21),
  324 MHz. Bank 0 broadcasts to the cores, bank 1 is the master, core 0 is at `+0x8000`.
  Reset: `MASTER_SOFT_RESET` `0x1ff`, SLC setup, then release. **powervr_sgx can't drive it yet.**
  Its open microkernel and compiler are USSE (SGX545) only, and the SGX544 is USSE2. Its
  DRM driver also targets Linux 7.3/x86. So the first step is
  `src/mainline/kernel/drivers/misc/mt5398_sgx.c`: an identify/reset driver that touches
  the GPU only on a sysfs write (`f0040000.gpu/action` = `id`, `reset` or `park`). The
  initramfs runs the action named in `<stick>/netcast/mainline/sgx`, after the watchdog is
  armed.
- **On the TV (boots 11-12, 2026-11-23).** Boot 11: every hardware check passed (header at
  `0x0e298080`: `e0000000 ff580000 e6000000 00000000 f0438780 10001000 00000438 40000780`,
  the fetched mirror matched, the CMA pool was at `0x1c000000`), but the probe failed with
  -22 because `drmm_connector_init()` refuses a connector with `.destroy`. Fixed. Boot 12:
  **mt5398_osd drives the panel.** The penguin appears and `/proc/fb` reports the DRM fbdev
  instead of simplefb. SGX `id` (boot 11): master and core 0 both read `0x01191101` / rev
  1.1.5, out of reset, all module clocks gated (idle), host events off. Reads don't hang.
- **Log gotcha:** Debian's `systemd-pstore.service` archives the previous ramoops record and
  erases it, and ramoops then zaps the live console zone, so the current boot's log before
  ~13 s is lost. The DTB now masks it (`systemd.mask=systemd-pstore.service`).
- Plane 3 is off after a watchdog return with no on-screen UI. nckexec used to refuse then.
  For the 1080p hand-off it now writes its own region header in the page after the bitmap
  (`0x25fe9000`), from the header boot 11 left (`e0000000 ff580000 e6000000 00000000
  f0438780 10001000 00000438 40000780`, with address and geometry filled in). It points
  plane 3 `+4` at that page, sets `+8` to stock's `0x0001ffff` and enables the plane, then
  strobes. With LG's UI up it still edits LG's header in place. The DTB's bitmap
  reservation covers only the bitmap (`0x7e9000`), so the header cookie's page doesn't
  overlap it. The own-header path first worked on the TV on 2026-11-24 (see the non-HOME
  bullet below). Before that fix the user reported (2026-11-24)
  that fire.sh doesn't work with no app loaded. So fire.sh now asks nckexec first
  (`echo layer > .../go`, which fails unless plane 3 is LG's 1280x720 layer). If it isn't,
  fire.sh starts `tools/ncboot` through `run-app.sh`: a libnc "Starting Linux" splash
  window. Once the splash has presented 3 frames it writes `/tmp/ncboot.ready`, and fire.sh
  checks the layer again before it goes on. If nckexec refuses, the splash gets SIGTERM.
- **Non-HOME screens: LG's UI is on plane 2 (2026-11-24, fixed and confirmed on the TV: that boot's fire.log says "LG's layer is on plane 2: plane 3 gets an own header", and Linux came up at 1080p with plane 3 header 0x25fe9000).** The user's fire.log said "OSD plane 3
  is off" at every failed check, with ncboot visible on the panel. Read live on stock 2 min after boot: plane 2 on
  with LG's 1280x720 ARGB layer (pitch 5120, headers `0x0e2a0040`/`0x0e2a0060` alternating), SC2 scaling it to
  1920x1080, plane 3 off, its last header a 64x64 bitmap (pointer?). When LG uses plane 3 instead is not known. `go=layer` now also accepts that layer on plane 2 for a
  1080p hand-off. nckexec then uses its own-header path on plane 3 and switches planes 1 and 2 off (not done before: LG's
  last picture could stay up). The plane 3 path is unchanged. fire.sh polls the layer for 10 s, `echo dump > .../go` logs
  the OSD planes, and fire.sh prints the reason when it gives up. ncboot re-raises its window every second. `osd_probe`
  also refuses a header in highmem.
- **Blending mapped (boot 15, `src/tools/osdalpha.c` + video, the tool and the driver's `dbg_*` params it drove are in commit 1b9da1b, removed after).** Plane 3 `+8` reads
  `0x0001ffff` (HFilter on, blend/fade ff). Per step, against the HDMI picture behind:
  - **pixel mix (1), AlphaSel 3:** correct per-pixel alpha (0 clear … ff opaque). This is
    stock's setting and the ARGB8888 default.
  - **none (0):** opaque. **region (2):** uniform weight, ff opaque, 80 half.
    **plane (3):** uniform, from plane `+8` (blend 80 = half).
  - **plane `+8` = 80/80 with pixel mix:** scales the per-pixel alpha.
  - **AlphaSel:** picks the byte that holds alpha (2 = red, 0 also remaps colour). Keep 3.
  - **XRGB8888 with region + ff (the default):** opaque.
  - Flips ran at 60 Hz: 2248 latched, 0 timeouts. Weston 14 (DRM backend, pixman,
    `LIBSEAT_BACKEND=seatd`) ran opaque with flower, simple-shm and terminal.
  - The earlier "flicker plus translucent picture" was `modetest -v`. It flips between the
    test pattern and a second buffer every vblank, so the eye sees flicker and a 30 fps camera
    averages the two buffers, which looks translucent. It is not a driver fault.
  - Driver fix: AlphaSel used to carry over whatever the header held (a debug override
    lingered). It is now set from stock on every flip.
  - **Boot 16 confirmed the fix:** step 12 w2 = `e5000000` (AlphaSel 3), blue row restored.
    Static `modetest` SMPTE (no `-v`) shows no flicker. In ARGB8888 only the middle row is
    see-through (libdrm gives it alpha 127), and XRGB8888 is opaque. Weston ran again
    (1776 flips, 0 timeouts), and the log has no warnings.
- **Desktop.** Xfce runs on Xorg. lightdm logs user `tv` in on vt7 without a password.
  `tv` is in the video, input, render and sudo groups and has passwordless sudo. Xorg
  uses the modesetting driver on `mt5398_osd`
  (`/etc/X11/xorg.conf.d/20-netcast.conf`), with no glamor. It renders on the CPU into
  a shadow buffer and draws the cursor in software. xfwm4's compositor is off (in
  `/etc/skel`). Firefox ESR is installed, and swap is on zram (lz4, half of RAM, kernel
  `CONFIG_ZRAM`). tty1 keeps the root console: Ctrl+Alt+F1 goes there and Ctrl+Alt+F7
  comes back. With `<stick>/netcast/mainline/console` or the `gfxtest` trigger present,
  lightdm doesn't start. The image is 3 GB because the stick is FAT, so the file must
  stay under 4 GB.
- **Release package.** `build/package-debian.sh` makes `dist/netcast-debian.zip`. It holds
  `netcast/mainline/` (`fire.sh`, `nckexec.ko`, Image, DTB, `debian.img`) and the
  stock-side tools (`ncpreflight`, `ncdebug`, `nckey`, `evdump`, `ncnetkeep`, `memscan`), with a README
  and licences (including a Debian package/source list). It has none of ncplus's apps.
  The image carries no SSH keys, and the script refuses one that does. At every boot,
  root's keys are copied from `<stick>/netcast/mainline/authorized_keys`
  (`netcast-sshkeys`). Before this, sync-in kept the host uid on the overlay's top
  directory, so `/` was owned by uid 1000 (now user `tv`). The build now chowns `.` too.

## Mainline audio: the stock DSP's MixSnd channel (2026-11-24, works: user hears the TV speakers)

First boot (2026-11-24, read over SSH): `DSP running (9/9 channel counters moving), channel 2 ring
0x3ef6ff00+2000, rp 0 wp 0`, card 0 "MT5398", PipeWire sink at 40%. Playback through
PipeWire works (7.5 s file in 7.6-7.7 s). Left and right are on the correct sides (one blind noise-burst
test, random sides, user right on both), but it was faint: see "Gain" below.

- The MediaTek driver is `mtk_mod` = `/mnt/vendor/dtv_driver_adpt.ko` (7.9 MB, **not stripped**, a copy in
  `tv_dump/audio/`, gitignored). It is not in the stock kernel image. It registers no reboot notifier, platform
  driver or syscore hook, so `nckexec`'s shutdown leaves the audio DSP alone. `/dev/kmem` is readable on stock
  (`dd if=/dev/kmem bs=4 skip=$((virt/4))`), which reaches the module's variables.
- DSP memory is ~5 MB in the RAM hole, `0x3ea57200`..`0x3ef5ff00` plus the MixSnd buffers up to `0x3effff00`.
  The DSP addresses a 16 MB window at `0x3e000000` (pointer words hold the 24-bit offset `<< 8`).
  `_u4DspBuf` (DSP shared memory) = `0x3ec0ca00`. Same addresses after a reboot.
- Decoder AFIFOs (`_u4AFIFO`): dec0 `0x3ee25f00`, dec1 `0x3ee6df00`, dec2 `0x3eeb5f00`, dec3 `0x3eefdf00`, 0x48000
  each. Write pointers: dec0 `0xf000005c` (parser block), dec1 `0xf000504c`, dec2 `0xf0005260`, dec3 `0xf000526c`.
  `AUDIO_BASE` = `0xf0005000`.
- `mtkaudiosink` uses `MTAUDDEC_PlayPCM_*`. By default (unless a debug-mask bit is set) that path is a **host
  software mixer** in `mtk_mod` (`_AudFeedMixSndThread`, `vMixer`, `do_SRC_*`): it resamples to 48 kHz stereo S16
  and `vSoftTransfer`s into one of 9 DSP "MixSnd" channels. Each channel has an 8 KB ring from `0x3ef6bf00` in
  steps of 0x2000, and a 64-byte control block at `0x3ec0fa00 + n*0x40`: +0 WP (host), +4 start, +8 end,
  +0x10 counter (ticks on idle channels), +0x20 RP (DSP). On idle channels RP == WP: the DSP stops at the write
  pointer, it does not loop.
- Measured with a test tone on stock (`audiotestsrc ! mtkaudiosink render-mode=1`, user heard it): only **channel 2**
  moved (ring `0x3ef6ff00`..`0x3ef71f00`). WP granularity 256 bytes.
- Mainline driver `src/mainline/kernel/sound/arm/mt5398-mixsnd.c` (patch 0006, DT node `sound`): takes the place
  of the feed thread for channel 2. RAM ALSA buffer (mmap works) plus a 5 ms soft hrtimer that copies whole 256-byte
  units into the ring and writes WP. 48 kHz S16 stereo only. Volume = the channel gain (see "Gain"). Since
  2026-11-24 the driver has `Master Playback Volume` (61 steps: 0 mute, 1..60 = -59..0 dB,
  TLV dB scale, at most `0x20000`) and `Master Playback Switch`. Probe starts it at -18 dB (`0x4075`) and logs the gain stock left. PipeWire
  drives it (seen on the TV, Image `805fcb5a`): slider 20/40/60/100% -> -41/-23/-13/0 dB -> DSP word 0x490/0x243f/0x729f/0x20000 (<<8), mute -> 0,
  `amixer` changes show in PipeWire. User's view of a 440 Hz tone: 20% ~ "35%", 40% ~ "70%" / "75% of what I'd ever get to", so the
  range is hot for a steady tone (music is quieter on average): top end may want a lower cap, e.g. -15 dB. Not changed yet. It refuses
  to probe unless the control block still describes that ring and the DSP counters move. Debian gets PipeWire
  (pulse + alsa), WirePlumber, alsa-utils, xfce4-pulseaudio-plugin and pavucontrol.
- **Gain (2026-11-24):** control block `+0x0c` = channel gain << 8 (unity `0x20000`: the stock setter's cap, `vSetMixSndGain`).
  `DSP_SetMixsndVolume` writes all nine, `gain = min(table[vol], table[100])` from a 101-entry volume table
  (`_AUD_DspMixSndVolume`). Read live on channels 0-3 and 8: `0x748`, about -37 dB. The user says the TV volume was low (5-15) before the
  boot, which fits, but a high-volume boot hasn't been compared. Writing it live (`busybox devmem 0x3ec0fa8c 32 0x00400000`, channel 2 only) was
  approved by the user: alternating `0x748` and `0x4000` (+19 dB) every 3 s during a steady 440 Hz tone was heard as "alternating loud and soft", so the
  word sets the loudness, with no DSP command. Restored to `0x748` afterwards. The first try (three steps with the same L/R/both tone) gave
  contradictory answers ("each clearly louder", then "about the same volume. It gets louder during the tone"). The toggle is the valid test.
- Recon helper: `tools/audrecon.sh` (read-only).

## Mainline Bluetooth keyboards (2026-11-24, pairs on the TV, link keeps dropping)

First boot (2026-11-24, read over SSH): an Actions dongle (10d7:b012, full speed, no firmware needed)
comes up as hci0. A BLE keyboard ("RT721 BLE Keyboard", HOGP via uhid) is paired, bonded, trusted.
It connected 82 times in 1000 s: gaps mostly 2-10 s, the last three 33 s apart (241 s before that).
The user says the keyboard works fine: set aside (2026-11-24).
bluetoothd logs "Request attribute has encountered an unlikely error" for the HID reads each time.
Cause unknown (btmon not used: it would record keystrokes).

- The Bluetooth stack is built as loadable modules, not into the Image: `bluetooth`, `btusb` (+ `btbcm`, `btrtl`,
  `btintel`, `btmtk`), `hidp` (classic BR/EDR keyboards), `uhid` (BLE/HOGP keyboards through bluetoothd),
  `ecdh_generic`/`ecc`, and AF_ALG for bluetoothd. `build-mainline.sh` runs `modules_install` into
  `out/modules/lib/modules/6.12.119/` (stripped, 1.1 MB, with `modules.dep`/`modules.alias`). `build-debian.sh` copies
  that into the image's `/usr/lib/modules`, so udev autoloads `btusb` when a USB adapter appears and `modprobe` works.
- Debian: `bluez` (`bluetoothctl`) + `blueman` (panel applet for pairing). No TV-internal Bluetooth adapter has
  been seen under mainline (USB ports: stick, Ethernet, LG Wi-Fi 043e:3005, port 1 empty), but a USB dongle works.
  Realtek and some Broadcom dongles want firmware files that are not in the image yet.

## Next steps

- On the TV: test Settings, Terminal, Remote Desktop and QML. Confirm the media player and RetroArch visually. Live-test nclite.
- Kernel modules: tun, cifs (check `input_dev`-style struct offsets against LG's code first, as for xpadone).
- Mute while the screen saver is dark (no extra procmgr registrations: those can't be unregistered).
- Investigate the intermittent audio-init segfault. Wi-Fi stalls after large uploads (try Ethernet).
- Wi-Fi: soak-test `ncnetkeep` over hours. Find why the firmware drops group-addressed frames (true root cause).
- Magic Remote pointer, ScummVM, a file browser.
- Newer kernel: research only (LG's display/GPU/Wi-Fi drivers are binary modules for 3.0.13). `nckexec.ko`
  warm-boots from RAM, but only ever try a non-stock kernel on a TV that passes `ncpreflight`.

## Project layout

| Path | Contents |
|---|---|
| `src/nctest/` | first GUI probe (`nctest.c`) and launcher (`run.sh`) |
| `src/libnc/`, `src/apps/`, `src/Makefile` | libnc runtime, demo apps, cross-build and deploy |
| `third_party/` | Khronos headers, SDL2 source + netcast patch + build script, prebuilt CMake, `sdl2-netcast/`, SDL2_mixer, Chocolate Doom, downloads in `dl/` |
| `build/package.sh`, `dist/` | ncplus package builder and output (`ncplus.zip`) |
| `build/` | BusyBox source and config, staged rootfs, `build-image.sh`, `netcast-root.ext4` |
| `usb/netcast/` | files deployed to the USB stick (image, BusyBox, `enter.sh`, `teardown.sh`, MD5SUMS) |
| `toolchain/` | buildroot-nc4 SDK |
| `tools/` | `tv.py`, `pull.sh`, `push.sh`, `netwatch.sh` |
| `src/kmod/` | out-of-tree kernel modules (nchello, xpadone) for the stock 3.0.13 kernel |
| `src/qml/hello/` | QML Hello World (static Qt 6) |
| `third_party/kernel/` | linux-3.0.13 (configured for vermagic) + Bootlin GCC 7.3 toolchain |
| `third_party/qt/` | Qt 6.12 sources, `build-host.sh`, `build-target.sh`, eglfs_netcast plugin (in the qtbase tree) |
| `third_party/freerdp-build/`, `lvgl/`, `libvterm-0.3.3/`, `retro/` | FreeRDP, LVGL, libvterm, RetroArch + cores |
| `tv_dump/lglib/` | TV libraries and binaries pulled for reverse engineering, plus disassembly (`*.S`) |
| `tv_dump/glibc/` | LG glibc 2.12.2 from the TV's `/lib` |
| `sdk_extract/` | carved SDK resource zip (deletable) |

## First look inside Debian on the TV (2026-11-24, over SSH, read-only)

Kernel #35, Debian 13.8, 736 packages, no failed units. 13.8 s kernel + 46.5 s userspace
(`blueman-mechanism` 36 s, the last thing before graphical.target). 720 MB RAM, 200 MB used with
Xfce idle, zram swap unused. Stick reads 11.8 MB/s (direct, 64 MB). Xorg: modesetting, shadow fb, no accel.
- SGX: first run of `pvrsrvinit` on the TV. It exits 1 with no message. The kernel then logs
  "BridgedDispatchKM: Initialisation failed. Driver unusable." and `eglInitialize` returns 0x3003.
  The "never mapped" mmap warning precedes it. Which bridge call fails isn't known.
  `pvrtest.sh` greps its own kmsg lines back (nested "pvrtest: dmesg:"). pvrtest.log has
  512 NULs and spliced text: most likely the musb write bug (next section), not a torn write.
- Network: DHCP gives an address and gateway but no DNS. Outbound TCP to 1.1.1.1 fails (by design: this machine is kept offline). So no NTP,
  and the clock is months behind (April instead of November).
- The internal Wi-Fi (043e:3005, Broadcom) is enumerated and has no driver or netdev. It re-enumerated
  once at 47 s as "Remote Download Wireless Adapter" (firmware-download mode).
- PipeWire has no rtkit, so no realtime priority.

## musb: wrong first packet of PIO scatter-gather writes (2026-11-24, fixed, booted on the TV)

Boot of the volume-control kernel (#36, Image 7e94f15c) oopsed at 29.8 s in `__raw_writesl` <- `mt5398_write_fifo` <- `musb_ep_program`,
usb-storage thread (reading 0xffeef000, pte 0), right after pvrsrvkm loaded and pvrtest.sh wrote its log to the stick: coincidence of timing,
not the GPU module or the audio driver. usb-storage died with the lock held, all stick I/O hung, systemd ran into its timeouts, and the
watchdog reset the TV to stock (log read back with a ramlog.py copy).
- Cause (upstream musb_host.c, unmodified): `buf = qh->sg_miter.addr + urb->sg->offset + urb->actual_length` although `sg_miter.addr` already
  contains the offset. The address fits: sg offset 0x800 -> page + 0x1000, the next (unmapped, highmem) page.
- Effect: only PIO writes with a nonzero scatterlist offset (sub-page, e.g. 512-byte FAT sectors and log tails). Whole-page writes are fine.
  With usb-storage only highmem pages get there (usb_sg_init() sets transfer_buffer for lowmem ones). Offsets below 0x800 give **wrong data in the first 512 bytes of the write** and no oops,
  from 0x800 the old address is in the next page and faulted in the test below. The 512 NULs in pvrtest.log fit. The FAT metadata on the stick is the main risk:
  check the stick with fsck.vfat on a PC before using it again.
  Done 2026-11-24 on the user's PC: `fsck.vfat -n` finds only the dirty bit and the matching boot-sector difference (offset 65, the state byte),
  no FAT or directory damage (55 files). File contents aren't covered by it. `e2fsck -fn` of debian.img (journal not replayed):
  all five passes clean. Only "orphan_present is set but orphan file is clean", expected after an unclean stop.
- Fix: `src/mainline/patches/0007-musb-host-pio-sg-offset.patch` (drops the second offset). Image 805fcb5a. Every earlier Debian-era boot ran the bug.
- Before/after on the TV (2026-11-25): test Image fe9863c5 = the 1.0 source and config plus a switch that sends the first packet of *tagged* writes
  from the old or the new address. O_DIRECT writes from highmem pages to a scratch file on the stick, read back and compared, 9 cases each way:
  old address: 5 wrong on the stick (sg offsets 0x200, 0x400, 0x600, always the first 512 bytes, holding the page data from twice the offset), 2 faulted
  (0x800 and 0xe00, probed only, not sent), 2 correct (offset 0). New address: 9 of 9 correct. One 0x800 write had the old address 0xffeef000, the one in the oops above.
  One kernel with both calculations, not two boots. An unpatched kernel wasn't booted (the root is on the same stick). Kernel log clean, untainted.
- Second upstream bug, same file (found 2026-11-25, fix not in our kernel): a half-done PIO bulk URB that musb_bulk_nak_timeout() set aside restarts wrongly.
  OUT is sent again from byte 0, IN stores the rest at the start of the buffer. Needs more busy bulk endpoints on one port than it has (5 TX here), so one stick per port can't hit it.
  TV test with *injected* restarts on a 2 GB spare stick (Image 912c9f9b): patched 9 of 9 correct. Unpatched: 3 of 3 reads wrong and reported as
  successful, the first write left packets 0 0 1 2 3 4 5 6 on the stick, gave an I/O error (both runs) and the stick needed a replug.
  Real timeouts (two sticks behind a hub, forced onto the shared endpoint, 40 s per phase): separate endpoints 3663 writes, no reset. Shared, patched: 5 mid-transfer timeouts, 262 writes all
  read back correct, but both sticks hung until the 30 s SCSI timeout, 5 USB resets, `Could not flush host TX1 fifo: csr: 0183` (FIFONOTEMPTY, TXPKTRDY, NAKTIMEOUT, DATATOGGLE).
  Shared, unpatched: 1 mid-transfer timeout, a reset 1.3 s later, 5 resets, 183 writes read back correct after retries. So the draft patch is **not enough** and the real test does not show data corruption.
  Not explained. Suspect (not tested): a packet the stick accepts while musb_bulk_nak_timeout() is rotating, or our glue.
- The volume-control part did start on that boot: "channel gain: stock left 0x748, now 0x4075".

## RAM corruption: something still writes to RAM Linux owns (2026-11-24, seen on #37)

After the musb fix boot, `dpkg --verify` reported 2 bad files that re-read fine: the on-disk data matches the host-built image, so the
*data in RAM* was wrong. A copy of libwireshark in /dev/shm changed by itself between checks: 2-3 bytes at one offset, ticking up.
A scan (300 MB of zeroed anonymous memory, pagemap) found one word that moves by itself: **phys 0x482a8150**, a counter at about 1600/s
(`aa2, 1743, 23e1, 3079, ...` every 2 s). 0x40000000-0x487fffff is System RAM for us (stock highmem). Any page there can be corrupted,
including data written to the stick. Cause unknown. Candidates: a leftover DMA master of LG's software, or the GPU microkernel left by the failed pvrsrvinit
(pvrtest runs on every boot, so this boot can't tell them apart). Second boot (2026-11-24, same Image 805fcb5a, `pvrtest` marker removed, pvrsrvkm not loaded, SGX untouched): the word is still there, at phys
**0x4819e150** (same page offset 0x150, two scans), which I first read as "not the GPU": **wrong, see below** (it only rules out *our* pvrsrvkm/pvrsrvinit). Counter 997.5/s over 4 s (1 kHz).
No interrupt has that rate (timer 120, musb 51/s). A userspace write to it seemed to stop it (stale cached copy), so the writer is not
cache-coherent: a device or microcontroller, not Linux code. It counts up in place (restarts from 0 after the page is zeroed and written back).
Both addresses are in the top 8 MB (0x48000000-0x487fffff). Only two samples, and the address differs per boot.
Mitigation to try: take that range (or the whole 0x40000000 bank) out of the DTS memory and rescan. Identifying the writer: look on stock
for a word that ticks at 1 kHz in highmem (dev/mem, kallsyms of dtv_driver_adpt.ko's .bss). Not yet scanned: kernel memory, pages in use.

**Identified (2026-11-24, read-only on stock): LG's SGX544 microkernel, owner `pvrsrvkm`.** `src/tools/ticscan.c` (three uncached /dev/mem
snapshots per MB) over 0x40000000-0x487fffff found 20 words ticking near 1 kHz. One has the same page offset: phys **0x45b05150**
(1002/s, 1018/s, started about 9 s after boot). Others are in groups that hold the uptime in ms (userspace timestamps).
The words around it match `SGXMKIF_HOST_CTL` (LG's layout from pvrsrvkm.ko DWARF) at 0x45b05120:
`+0x00 ui32InitStatus` = 1, `+0x14 ui32HWRecoverySampleRate` = 10 (1000/100 Hz), `+0x18 ui32uKernelTimerClock` = 0x4f1a0 = 324000
(324 MHz / 1 kHz), `+0x20 ui32InterruptFlags` = 4, `+0x1c ui32ActivePowManSampleRate` = 0 (no active power management),
`+0x30 ui32TimeWraps` = the counter. So the 1 kHz EDM timer task of the stock microkernel keeps running after the warm boot: nckexec
stops USB DMA but not the GPU. `CLKGATESTATUS` 0 in boot 11's `id` only meant idle at that instant.
The page differs per boot (0x45b05, 0x4819e, 0x482a8: ordinary highmem pages), so **reserving a fixed range would not have helped**
(the stock one is not even in the top 8 MB). The GPU's page tables and microkernel code/data pages are also in RAM we reuse, so once they are overwritten
it could in principle write elsewhere (inference, only the counter has been seen).
**Fixed (2026-11-24):** `sgx_stop()` in nckexec.c (param `gpustop`, default on), right after `musb_dma_stop()`: waits up to 50 ms for core 0's
`CLKGATESTATUS` (0xf0048008) = 0, then writes `MASTER_SOFT_RESET` (0xf0044080) = 0x1ff and reads it back. Booted on the TV (Image #37, new
nckexec.ko staged in /tmp with fire.sh, no pvrtest). Under Debian: register reads 0x1ff. Two 300 MB scans and one 440 MB scan of zeroed memory:
0 words changed. `ticscan` over all of 0x40000000-0x487fffff and 0x0-0x25ffffff (pages in use included): 0 words ticking near 1 kHz. `dpkg --verify`:
no content mismatch. Untainted. GPU bring-up from the held-in-reset state: next paragraph. Untested: and `mt5398_sgx`'s `id`, which reads core-bank registers in reset.
**GPU bring-up from held-in-reset, first try (2026-11-24, same boot, pvrtest.sh run from /dev/shm, nothing written to the stick):** insmod is fine, then the
*same* failure as on earlier boots: `pvrsrvinit` exit 1, "psOffsetStruct was never mapped", "Initialisation failed. Driver unusable", and
`MASTER_SOFT_RESET` still reads 0x1ff afterwards, so SGXReset() never ran. **The failure is in software, before any GPU access, and has nothing to do with the
reset.** Cause (source-level): mmap handles start at FIRST_SPECIAL_PFN 0x80000000 pages (PVR_MAKE_ALL_PFNS_SPECIAL). Since 4.17 do_mmap() refuses
offsets above 4 GB on a character device (-EOVERFLOW) unless the file_operations set `FOP_UNSIGNED_OFFSET`. libsrv_um maps through
`syscall(__NR_mmap2)` with that 32-bit page offset. Fix: patch 0004 in src/mainline/sgx/patches, pvrsrvkm.ko md5 85ac0bcb, **not yet run on the TV**. Earlier pvrtest
failures (exit 1 on every boot) fit the same cause. Whether the GPU itself comes out of reset is therefore still unknown.

**Second try, with patch 0004 (pvrsrvkm 85ac0bcb, from /dev/shm after a clean rmmod of the failed one):** the mmap fix works: pvrsrvinit now gets to
PVRSRVInitSrvDisconnect -> SysFinalise, and there it **oopses: imprecise external abort reading core register 0x24 (CORE_REVISION, bank 0)
in SysCreateVersionString**. Core-bank reads abort while the GPU is held in reset (the insmod read nothing there). After it, even the
master register 0xf0044080 aborts (devmem: SIGBUS), which it did not before. The GPU bus slave looks stuck until the next reset. pvrsrvinit is left in D state.
The kernel survived, a 300 MB scan has 0 hits. So: **a driver can't start from "held in reset" without first releasing the reset** (LG's stock code is the same, but there the GPU is not in reset), and the `id`
action of mt5398_sgx would oops too. Fix in the driver: SysFinalise() runs SGXReset()'s master steps and releases the reset before the first core read
(src/mainline/sgx/system/mt5398/sysconfig.c, SysReleaseReset()).
- **Third try: GPU 3D works (2026-11-24, fresh boot, GPU held in reset by nckexec, pvrsrvkm 93ff4fe9 from /dev/shm):** "MASTER_SOFT_RESET was 0x1ff",
  "SGX revision = 1.1.5", `pvrsrvinit` exit 0, `sgxgltest` PASS: EGL 1.4 and "PowerVR SGX 544MP" OpenGL ES 2.0, build 1.9@2291151, clear and
  shaded triangle correct (frame looked at on the host), 70 SGX interrupts. So LG's microkernel now runs under mainline. The test's old fps figure (85800)
  was not a measurement (the tile GPU drops frames nobody reads). The benchmark below replaces it.
- **GPU benchmark (`sgxgltest -b`, 1280x720 pbuffer, three runs within 2%, every checked frame's pixel value is right, additive blending so each layer must have been drawn):**
  flat blended fill 550 Mpixel/s (10 layers) / 579 (30 layers), textured the same (545 / 590): about 595 Mpixel/s net of ~2 ms frame overhead.
  4.0 Mtriangles/s (40000-triangle grid x 6 passes, unindexed). Clear + 1-pixel readback 1.9 ms (520-540/s). **Full-frame glReadPixels 250 ms (14 MB/s):
  the CPU path out of GPU memory is slow, so an accelerated display must scan GPU memory out directly, not read it back**.
  **`glFinish` does not wait** (20 blended layers: 0.2 ms, then the next readback takes 63 ms, which is the fill cost of 40 layers). Sync with a 1-pixel readback.
  about 750 SGX interrupts per run (70 -> 817 -> 2313 over one then two runs), no recovery or fault messages, 300 MB scan after it: 0 hits.
- Scans with the microkernel running: 300 MB zeroed scan 0 hits. Full-RAM ticscan: our own HostCtl (0x0f033150: same struct, in memory pvrsrvkm owns, not stray)
  and one kernel pointer (0x012877e0, steps of 0x80).

The pointer walk via /dev/kmem to double-check (gpsSGXDevNode = 0xcd967800 -> pvDevice -> psSGXHostCtl) was not completed: the TV stopped answering
telnet right after it and was back several minutes later freshly rebooted (uptime 48 s, by itself or power-cycled: unknown). Likely cause: the shell walk turned
an empty read into a bad /dev/kmem address (not confirmed). Do not repeat it. The struct match is enough.
