# LG NetCast TV hacking — findings

Last updated: 2026-11-20. `telnet <TV IP>` gives a root BusyBox shell (no login); the host tools read the IP from `$NETCAST_TV` or `tools/.tv-host`.

**Status:** a native C program built here draws GPU-accelerated graphics on the TV screen
(EGL + OpenGL ES 2, 1280x720 at 57 fps), above LG's UI. Confirmed visually.

## Ground rules

- **Never write to the TV's internal eMMC** (a modified system partition fails the boot-time signature check and hangs the TV; see "Flash, boot chain and secure boot"). **Always upload to the TV through the temporary busybox ftpd** (`tools/push.sh`). Only the USB stick
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
| kexec | not compiled in (only the weak `sys_kexec_load` stub); `nckexec.ko` does it from a module (see Kernel modules) |
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
- Run: `sh /mnt/usb/usb2/Drive1/netcast/apps/doom/doom.sh`. Back is Escape (see Remote keys).

## NetCast+ package (launcher + apps)

- `build/package.sh` builds `dist/netcast/` and **`dist/netcast-plus.zip`** (about 12 MB, FAT-friendly).
  Unzip to the root of a FAT32 stick, then run `sh /mnt/usb/usb2/Drive1/netcast/start.sh` from telnet.
  It detaches with nohup and logs to `/tmp/netcast.log`.
- `start.sh` loops: `system/launcher` (tiles from `apps/*/app.ini`) -> the chosen app's `exec=` script -> launcher.
  Only one process owns the HOA window at a time.
- `app.ini` keys: `title=`, `desc=`, `exec=`, `color=` (hex RGB), `order=`.
- Still needed: autostart without telnet (needs a persistent hook = flash write, excluded); Magic Remote
  pointer; more apps.

## More apps / screen control (2026-11-20)

- **OpenTyrian** v2.1.20260913: plain Makefile, built with `SDL_CPPFLAGS/SDL_LDLIBS` overrides against the static
  SDL2. Data is `tyrian21.zip` from camanis.net (freeware release; the zip still contains the 1995 EULA).
  Bundled only with `build/package.sh --with-tyrian-data`. Preset `opentyrian.cfg`: fire=Return,
  change fire=F1, sidekicks=F2/F3, Scale2x, 4:3.
- Back is Escape and CH- is PageDown in the SDL driver. Doom's cfg uses 27 for menu, back and abort.
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

## ffplay, Web Browser (NetSurf) and the test folder (2026-11-20)

- **ffplay** (FFmpeg 9.1, `build-deps.sh ffmpeg`): static except glibc, NEON software decode, GLES2 output through
  SDL. It handles what the hardware Player can't (VP9, HEVC, Opus, AAC in MP4). It has no launcher tile:
  Network Shares opens a video with YELLOW (`apps/ffplay/run.sh <file|URL>`), and OK still opens the hardware Player.
- **NetSurf 3.11** (`build-deps.sh netsurf`, framebuffer frontend on our SDL2 libnsfb surface). Build gotchas:
  the browser's own Makefile ignores `HOST` and only takes `CC`; without it the framebuffer frontend falls back to
  the host `cc` and links x86 objects. `NETSURF_CONFIG="CC=… CXX=…"` passes them to that sub-make only. NetSurf
  adds plain `-lz -lexpat`, and the TV has zlib 1.2.3 (no `ZLIB_1.2.3.4` symbols) and no expat, so the static
  archives are linked in from a `-L` dir that holds no `.so`. Result: needs only glibc ≤ 2.10.
- **NetSurf runtime:** `--option=value` arguments are only parsed *before* all others (`-w`, `-h`, URL).
  `NETSURFRES` = `apps/netsurf/res` (Messages, CSS, DejaVu fonts, `Choices`); toolbar images are compiled in.
  HTTPS needs `--ca_bundle` (the TV has no CA store; the package ships Debian's bundle). The "undefined
  instruction" lines in dmesg at startup are OpenSSL probing CPU features (SIGILL is caught), not a crash.
  Exit: a 5 s watchdog plus `_exit()` after teardown, like ffplay, because LG's destructors can hang.
- **No internet on the TV**, and DNS is broken too (the 127.0.0.1 resolver isn't listening, at least after the
  warm boot). `tools/webtest.sh` runs the static BusyBox `httpd` on 127.0.0.1:8090 over `netcast/test/`
  (browser test pages, synthetic media from `build/make-testmedia.sh`, test photos with a static index, since
  BusyBox httpd has no directory listing). It writes `/tmp/webtest.conf` with MIME types; without it, `.mp4` and
  `.mkv` are served as `text/html`. Range requests work (206). Player, Photos and the Web Browser start it when
  netcast.conf (or a URL argument) points at 127.0.0.1:8090.
- **Hardware decode, measured on the TV** (`gst-launch-0.10 playbin2` with `mtkvideosink`/`mtkaudiosink`,
  and ncplay): **H.264 (Main or High) + MP3 in MPEG-TS plays cleanly.** H.264 + AAC in TS plays video only
  ("Missing element: MPEG-4 AAC decoder"); MPEG-2 video: "Missing element: MPEG-2 Video decoder"; H.264 + AC-3
  in TS and MPEG-2 + AC-3: data-flow/not-negotiated errors; **any MP4, even H.264 with no audio, fails in
  qtdemux**. This contradicts the element list above (omx_mpeg2dec, omx_aacdec, omx_ac3dec exist): they don't
  link in this setup. The hardware test clip is therefore H.264 + MP3 TS; everything else goes to ffplay.
- **ffplay, measured:** VP9 + Opus 360p30 in sync (A-V within ±0.02 s), 33/450 frames dropped; HEVC 360p30 in
  sync but 238/450 dropped (too slow, so no HEVC test clip). Music plays FLAC/MP3/WAV via mtkaudiosink 44.1 kHz.
- **procmgr slots leak (the cause of "RegisterService failed"):** `HOA_PROC_UnregisterService` sends one bus
  request with our pid; procmgr never answers it (rc=-1, even as the first teardown step, while other procmgr
  queries on the same connection succeed). `HOA_PROC_GetRunningProcessCount` grows by one per app run and
  never drops (32, 34, 35, ...; `NotifyAppTermination` doesn't help). After roughly a dozen launches (each
  return to the launcher counts) registration fails for every name and NetCast+ exits; only a power cycle
  clears it. libnc has `NC_PM_DEBUG=1` (logs the count) and `NC_PM_NOTIFY=1` (experiment) for this. Not fixed.
- Tested on the TV (cold boot, user watching): the Web Browser renders the test pages (CSS, PNG, links,
  scrolling, Back) and ffplay plays the software test clips; the user signed both off.

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

- **Build:** vanilla linux-3.0.13 (LG's GPL source for this model isn't available) with the Bootlin armv7-eabihf
  2018.11 toolchain (GCC 7.3; `compiler-gcc7.h` copied from gcc4). Config: vexpress_defconfig plus SMP, PREEMPT,
  MODULE_UNLOAD, AEABI, VFP/NEON, VMSPLIT_3G (PAGE_OFFSET 0xC0000000), no MODVERSIONS (LG's modules have no
  `__crc_`). vermagic matches: `3.0.13 SMP preempt mod_unload ARMv7`. `make -C src/kmod` (`KCFLAGS=-fno-pie -fno-pic`).
- **`struct module` matches** LG's (`tools/kmod-check.sh` against `tfat.ko`: 0x140 bytes, init +0xbc, exit +0x138).
  `nchello.ko` loads and unloads cleanly from `/tmp`.
- **LG's `struct input_dev` is 136 bytes larger** between `keybit` and `open`: `open`/`close` at 500/504 and `dev`
  at 560 (vanilla: 364/368/424). `name`, `phys`, `id`, `evbit`, `keybit`, `struct urb` and `usb_device` up to
  `descriptor` match. Modules must not touch `input_dev` fields past `keybit`. Found by disassembling LG's
  built-in `xpad_probe`, read from `/dev/mem` (phys = virt − 0xc0000000). The `struct module` check does not catch this.
- **Built into LG's kernel:** xpad (original Xbox and Xbox 360 pads, rumble; no Xbox One), usbhid, hid-sony.
  A PS3 pad (`054c:0268`) works as is. More 360-protocol pads: `echo VID PID > /sys/bus/usb/drivers/xpad/new_id`.
- **`xpadone.ko`** (`src/kmod/xpadone.c`): Xbox One/Series wired pads (GIP, interface `ff/47/d0`), touching only
  the matching fields. These pads also have device class `0xff`, so the ID table matches vendor + interface
  (on 3.0 a bare `USB_INTERFACE_INFO` never matches); other vendors:
  `echo VID PID ff > /sys/bus/usb/drivers/xpadone/new_id`. Tested with a PowerA `20d6:2001`: all buttons, sticks
  (±32768), triggers (0–1023), d-pad (hat), rmmod/insmod. Guide hasn't been seen yet. `start.sh` loads it only on
  the kernel build it was checked against.
- `src/kmod/xpad.c` (vanilla xpad) is not shipped: it duplicates the built-in driver and writes `input_dev` at
  vanilla offsets.

### Warm boot of an unsigned kernel from RAM: `nckexec.ko` (working, 2026-11-20)

U-Boot only RSA-checks a kernel it loads from flash. `src/kmod/nckexec.c` hands the running kernel over to
another kernel in RAM, so the next kernel is never checked, and no flash write is involved.
**Tested on this TV:** booting the stock kernel through it gives a fully working system. It was back on telnet
~70 s after the jump, LG's RELEASE was running, all 726 MB of RAM was present, and the remotes reconnected.
Proof that it came from our handoff and not a reset through U-Boot: `/proc/cmdline` ends in the added
`maxcpus=1 panic=5`, only one CPU is online, and printk timestamps start at ~1079 s while uptime is ~74 s
(the SoC timer was never reset).

- **Kernel image:** p21 is an MTK LZ4P container (`tools/unlz4p.py`): a header, the u32 compressed size of each
  64 KiB block, then raw LZ4 blocks. It holds a uImage (load/entry `0x7fc0`, U-Boot `bootm 0x7FC0`) around a raw
  ARM `Image` linked at phys `0x8000` (`stext` 0xc0008000, PHYS_OFFSET 0). It matches the running kernel's text
  byte for byte. machine_desc: nr 5398 `MT5398`, `boot_params` 0x100, `fixup` NULL. The tag table has only the
  standard ATAGs.
- **Staging (`insmod`):** the image goes into single pages, all above 16 MB (so none lies in the destination;
  no large contiguous allocation). ATAGs are built fresh: CORE, MEM per `System RAM` bank
  (`0x26000000@0`, `0x08800000@0x40000000`), and CMDLINE (this boot's line plus `append=`; busybox insmod
  drops quotes, so `append=` takes `;` between arguments). A {dest, src, len} list puts the ATAGs at `0x100` and
  the Image at `0x8000`.
- **`go`:** first `kernel_restart_prepare()`, which runs the reboot notifiers and `device_shutdown()` (stops USB
  DMA). Then IRQ/FIQ off, `setup_mm_for_reboot()` (1:1 map), and a flush of L1 and the PL310. A stub at its
  physical address, using no stack and no kernel code, then does: D-cache off; L1 clean+invalidate by set/way;
  PL310 clean+invalidate by way; MMU, I-cache and branch prediction off; copy; enter with
  `r0=0, r1=5398, r2=0x100`.
- **PL310:** r3p2 (`0x410000c8`), 16-way, already enabled; the kernel maps it at VA `0xf2000000`. `l2x0_base` is
  not in kallsyms; its address comes from the literal pool of `l2x0_disable` (`ldr r5, =&l2x0_base`), and
  `l2x0_way_mask` follows it. `__l2x0_flush_all` is a plain way operation with no debug-register writes.
- **CPU1:** it has to be offlined first (`echo 0 > /sys/devices/system/cpu/cpu1/online`). The stock
  `platform_cpu_die` flushes L1, clears the ACTLR SMP bit and SCTLR.C, and waits in WFI for `pen_release`: a
  clean park. The next kernel must use `maxcpus=1`. Releasing CPU1 would send it back into the old kernel's
  hotplug code, and bringing up the second core under the new kernel needs a real holding pen.
- Module gotchas on this tree: no `%pa` (it arrived in 3.8). The vexpress `PLAT_PHYS_OFFSET` has to be patched
  to 0 (`build-deps.sh`), otherwise `virt_to_phys` is off by `0x60000000`.
- Run: `src/kmod/nckexec-fire.sh`, copied to the stick next to the module and the image (`sync`, CPU1 offline,
  `insmod … 'append=maxcpus=1;panic=5'`, `go`).
  If the new kernel fails it panics, and `panic=5` reboots through the normal signed boot.

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
  Vol- 114, Mute 113, OK 28.
- libnc maps 412 to KEY_ESC for every app. The SDL driver maps Escape, so Back is back/menu everywhere, and CH-
  is now PageDown. Photos and Player no longer treat CH- as back; the terminal keeps CH- = Ctrl-C.
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

## Flash, boot chain and secure boot (2026-11-20)

Dumped read-only to `tv_dump/flashdump/` with `tools/tv-dump-flash.sh` (BusyBox `dd` to the USB stick, served by
BusyBox `httpd`, md5-verified): p1 boot, p2/p3 PART.INFO, p4 swue, p6/p7, p8 U-Boot env, p13 key/DRM blob,
p16 rootfs, p17 lginit, p18 model, p20 LZ blob, p21/p22 kernel, p28 DRM ELF. U-Boot was analysed by LZHS-decoding
p1 at `0x80000` (port of openlgtv `epk2extract`) and disassembling at VMA `0x800000`.

**Summary: the boot chain RSA-verifies the system partitions on every boot. A modified rootfs, kernel, lginit or
lgapp hangs the TV at boot and, in RELEASE mode, permanently burns the "full verify" eFuse. There is no A/B
fallback. Nothing is ever written to flash; everything runs from the USB stick.**

### Storage

- eMMC only: `/dev/mmcblk0`, ~3.8 GB, 63 partitions (no NAND/MTD). System partitions are read-only squashfs
  (`root=/dev/mmcblk0p16 rootfstype=squashfs`). No dm-verity or IMA; `modules_disabled=0`.
- Writable ext4: p62 (`cmn_data`, `home`, `cache`, `user`, `flash/data`), p61 `system_app`, p63 `appstore`,
  p15 hdcp, p12 ciplus, p31 vcs.
- **PART.INFO** (p2, byte-identical backup `mapbak` = p3) maps logical names to physical extents. Header magic
  `16 07 12 20`, device `mtk3598-emmc`. 96-byte records: `+0x00` name, `+0x20` physical offset and size,
  `+0x30` filename, `+0x50` image size, `+0x54` version (`0x04031001` = v04.03.10), `+0x58` flags, `+0x5c` type.
  A CRC over the first 0x3050 bytes is stored at `+0x3050`. Per-partition CRC32s are in `CRC32.INFO`.
- **Banks:** updatable partitions (rootfs, kernel, lginit, swue, LG app/res) have two records. The active one
  has flags bit0 set (`0x101`, inactive `0x100`). U-Boot uses the first active record per name to build
  `root=`, `lginit=` etc. The inactive records hold the previous firmware, v04.03.09.

### p1 (`boot.bin`)

`0x0` MTK ROM preloader (MSDCBOOT, LZHS checksum, prints "not secure loader") · `0x9c00`/`0x9d00` header plus
0x300 bytes of signature data · `0xa000` "X935" header, LZHS at `0xa040` → MT5398 **Lmain** loader (0x52800 bytes,
Thumb, base `0xd00000`, DRAM init) · `0x33480` `mt5398_LGE_linux` tag · LZHS at `0x80000` → **U-Boot 1.3.4**
(0x853e0 bytes, ARM, `TEXT_BASE=0x800000`, prompt `NC4_M13 # `, built Jan 29 2014).
U-Boot env (p8): `autoboot=y`, `bootdelay=0`, `silent=y`, `verify=n`, `bootcmd=` (empty), no `watchdog=yes`.
`verify=n` does not disable the signature checks below.

### Preloader / BROM stage — where the chain of trust actually roots (2026-11-20)

The RSA checks in `verify_apps` protect everything *above* U-Boot, but nothing in flash protects U-Boot, Lmain
or the preloader themselves. Whether the whole chain can be bypassed comes down to one thing we cannot read from
flash: does the MT5398 mask BootROM authenticate the first flash stage? What the dump does tell us:

- **The flash-resident first stage is MediaTek's *non-secure* loader.** At `p1:0x100` is `MTK/DTV/ROMCODE/
  MSDCBOOT/V01.00` → `Start Pmain`. Its entire message table (`0x4398`–`0x4410`) is integrity-only: `EMMC boot`,
  `LZHS addr:`, `LZHS size:`, `LZHS checksum:`, `LZHS ok`/`LZHS failed`, and the mode banner **`not secure
  loader`** (`0x43cc`). A scan of the whole 1 MB p1 for `efuse`/`secure boot`/`RSA`/`authenticate`/`sign`/`SBC`/
  `OTP`/`public key` returns **only** the `secure loader` substring of that banner. So MSDCBOOT loads the next
  stage (Lmain, the `X935` blob at `0xa000`) and checks a **checksum, not a signature**.
- **p1 nonetheless carries its own signature structure** at `0x9c00` (magic `96 53 68 53`, a pointer `0xf000d3e4`
  into SRAM, a `02` field, then ~0x300 bytes). A signature over the loader exists in flash, so *something* was
  meant to verify it — and it is not MSDCBOOT (the non-secure loader) and not `verify_apps` (p1/boot.bin is **not**
  in that list, see below). The only remaining verifier is the **in-SoC mask BootROM**, which is not in flash.
- **Consequence.** If the BootROM does *not* enforce that signature — i.e. the MediaTek secure-boot (SBC) efuse is
  unblown on this SKU, which the shipped *non-secure* MSDCBOOT strongly suggests — then a modified p1 (preloader →
  Lmain → U-Boot) would boot, and that defeats the entire `verify_apps` RSA chain, because nothing above re-checks
  the bootloader. If the SBC fuse *is* blown, it does not, and p1 cannot be altered. This is the single unknown
  that decides whether a newer kernel/OS is loadable on this hardware.

**Lmain decompressed (2026-11-20).** `tools/unlzhs.py` (a from-scratch LZHS decoder, Huffman+LZSS, ported from
openlgtv epk2extract; reproduces Lmain `0x52800` and U-Boot `0x853e0` byte-exact) was used to decompress both stages
for analysis. Findings in Lmain (Thumb, base `0xd00000`):

- Lmain **does** chain-verify the next stage: a crypto/bignum routine (dispatch at `~0xd2de90`–`0xd2e06c`) feeds a
  check that prints **`secure boot ok`** / **`secure boot fail`** (strings at va `0xd44c5b`/`0xd44c49`, decision code
  near `0xd13980`). So LG's design authenticates stages *below* U-Boot too, not only `verify_apps`.
- The efuse/OTP bank `0xf0008000` is referenced throughout Lmain, including the full-verify OTP `0xf0008664` that
  U-Boot also reads. The exact bit that *enables/bypasses* secure boot (the MediaTek SBC fuse) has not been pinned
  down by static disassembly yet — the verify path is a large switch and hand-tracing it reliably needs more work;
  do not assume enforced or bypassed from the strings alone.

**Non-destructive confirmation (read-only, safe):** read the efuse/OTP bank with `devmem` — the full-verify OTP at
`0xf0008664` bit0 is already known-readable, and the surrounding words (`0xf0008660`–`0xf00086a0`, plus the
`0xf0008670` block Lmain hits ~29×) are the place to look for the SBC enable bit. Reading fuses changes nothing.
**Testing** a modified loader, by contrast, means writing the p1 boot partition: that violates [[tv-no-flash-writes]]
and risks the exact brick + permanent-full-verify outcome, so it belongs on a sacrificial unit with a proven
UART/JTAG recovery path, never on this TV. SHA-1 in the upper chain (below) is collision-weak but not a practical
forgery path; whether the SBC fuse gates Lmain's (and the BROM's) verify is the real question.

**devmem dump of the OTP/efuse bank (2026-11-20, read-only, this TV).** Physical `0xf0008600`–`0xf00086ff`
read back word-by-word with `/sbin/devmem <addr> 32` over telnet. Non-zero words only (all others `0x00000000`):

```
0xf0008664: 0x70180004   full-verify OTP  (bit0 = 0)
0xf0008670: 0x6FFE01F7   SKU feature fuse word (NOT secure boot — see below)
0xf0008674: 0x00555653   "SVU" (ASCII, LE)
0xf0008688: 0x00001404
0xf000868c: 0xE11D07D8
0xf00086b0: 0x00000002
0xf00086b4: 0x80CD9000
```

- **Firm result: the permanent full-verify latch is NOT set.** `0xf0008664` = `0x70180004`, bit0 = 0. This TV has
  not been tripped into the always-RSA-verify / brick state, which is consistent with it booting normally and with
  `verify=n` in the U-Boot env. So the *catastrophic* OTP outcome from a failed image check has not happened here.
- This is the live MMIO read-back of the OTP — the same source Lmain/U-Boot read — not the bootloader *image*
  (that is p1 in `tv_dump`, never to be rewritten per [[flash-secureboot]]/[[tv-no-flash-writes]]).

**Lmain fuse-decode trace (2026-11-21, static RE of the decompressed `lmain.bin`, `tools/unlzhs.py` offset
`0xa040`; capstone Thumb). This CORRECTS the earlier "`0xf0008670` is the secure-boot word" guess:**

- **`0xf0008670` is the SKU *feature* fuse word, not secure boot.** Its "29 references" are 28 auto-generated
  per-bit accessor functions (28-byte blocks at `0xd0da5c`+, each `ubfx r0, [0xf0008670], #bit, #1`, descending
  from bit 28). The fuse-dump routine at `0xd1a1f4` prints them by name — `IS_SupportDOLBY`, `DTS`, `VC1`,
  `DivXHD`, `3DGPU`, `HBBTV`, `MHP`, `GINGA`, `ARIB`, `JVM`, `OPERA`, `MHEGHD`, `CPU1G`, `RM`, `AVS` (strings at
  `0xd460df`+). Pure codec/middleware licensing bits.
- **There is no secure-boot *enable* fuse that Lmain reads.** A token sweep of Lmain's whole string table
  (`secure`, `SBC`, `authenticate`, `verify`, `fuse`, `RSA`, `signature`, `trust`) returns only `secure boot ok`
  / `secure boot fail` (va `0xd44c5b`/`0xd44c49`) and `MT5398 Boot Loader`. Every named fuse accessor is an
  `IS_Support*` feature bit; **no `IS_SecureBoot`/`IS_SBC`/auth accessor exists.**
- **Lmain verifies its next stage unconditionally.** The verify wrapper at `0xd13980` calls the crypto routine
  `0xd2dec8`, branches to "secure boot ok"/"secure boot fail" (fail → returns 2), and only then drives the
  hardware **LZHS decompressor** (regs `0xf00080b0/b4`, `0xf00081d0`; strings "LZHS start/done", "decode checksum
  is not the same"). The `0xf0008000`–`0x81ff` block is the **eFuse/crypto controller** (command/data/status at
  `+0x00/04/1c/34/74/78`), not enable bits. Authentication is not gated behind a readable fuse the way features
  are.
- **Conclusion: the chain's secure-boot on/off switch is not in Lmain** — Lmain always authenticates U-Boot. The
  enable/bypass decision lives in the **mask BootROM** (whether it enforces p1's `0x9c00` signature over Lmain),
  which is not in flash and not symbolically present here. So the SBC question cannot be answered by reading a
  single fuse; the only named enforcement fuse in reach is the full-verify OTP `0xf0008664` bit0 (= 0, read above),
  which controls partial-vs-full signature checking + the burn-on-fail latch, *not* "is secure boot enforced."
- **For debugging, this changes nothing actionable:** the reversible levers stay the EEPROM debug level
  ([[tv-debug-level]]) and `nckexec` ([[nckexec-warm-boot]]); a definitive BROM dump needs MTK BROM download mode
  on a sacrificial unit with hardware access, not `/dev/mem` on this TV (the `0xf00080xx` controller space returns
  erroring/side-effecting reads when probed live).

### Image signatures

Each image is the payload (4 KB-padded) plus a 0x1500-byte trailer of 21 RSA-2048 signatures. Signature k
(0–19) covers 4 KB blocks k, k+20, k+40, …; signature 20 covers the whole image. SHA-1 + RSA public decrypt
(`0x8235f4`). The modulus is read from SRAM `0xfb005120`, which Lmain copies from SRAM `0xf000`; it is not in
p1 in plaintext.

### Boot sequence (`0x8006d0`)

1. Fast-boot check: except in RELEASE, a 200 ms window in which a backtick on the UART opens the U-Boot console.
2. Boot state (`0x81bc64`): normal, cold boot that makes a snapshot, or snapshot resume (`lgsnap`, 64 MB `hib`).
3. `verify_apps` (`0x8239dc`): lginit, rootfs, tzfw always; vendor, opensrc, lglib in normal mode; patch,
   emanual, estreamer, extra, base only with full verify; swue in SWU mode. Without full verify (OTP
   `0xf0008664` bit0) it checks one partial signature per partition, chosen per boot (`[0xf0008074] % 20`).
4. SWU mode (EEPROM: `0x20121205` at NVM `0x198` and `'S'` at `0x18b`): `cp2ram swue 0x04000000;verification 0 swue
   0x04000000;` + kernel, which boots the update engine to resume an update.
5. Otherwise: `cp2ramz lz4 kernel 0x7000000 0x7FC0;verification 0 kernel 0x7000000;xiplz4 lgapp;bootm 0x7FC0`.

| Failure | Behaviour |
|---|---|
| PART.INFO CRC bad | U-Boot copies mapbak (p3) over p2 and continues (mapbak's CRC is not checked first). |
| Signature mismatch | `signature check fail!`; in RELEASE burns the full-verify eFuse (`0x823394`) and spins forever (`0x8236b4`). No reset, no bank fallback. |
| RSA decrypt error (bad padding) | Returns -2; boot continues. |
| Snapshot image bad | RELEASE: erases `hib` and resets into a cold boot. DEBUG: hangs. |
| Kernel load or `bootm` fails | Falls to `main_loop` with an empty `bootcmd`: sits at the `NC4_M13 #` prompt. The HW watchdog (`0xf0028100`) is armed only with `watchdog=yes`. |
| Kernel panic or init dies | `panic=1`: reboots after 1 s into the same bank, i.e. a boot loop. No Linux watchdog device. |
| Interrupted update | The SWU flag stays set, so the next boot resumes the update in swue. |

Nothing counts failed boots or tries the inactive bank; only swue switches banks.

### Updates (swue, p4)

Mounted only during an update. EPK packages are **RSA-signed and AES-encrypted** (swue bundles
`libcrypto.so.1.0.0` and `netflix_pub.pem`). Flow: decrypt and verify the EPK → write the images to the inactive
bank → `API_SWU_VerifyImage` CRC32-checks them (on mismatch: no swap, `API_DDM_POWER_Shutdown_ByVerifyFail`) →
`API_SWU_SetCRC32` and version update → "Boot Swap" flips the active flags in PART.INFO and mapbak → reboot.
`CRC32.INFO` is used only here. A failed second verify also sets the OTP ("OTP will be set and System will be
shutdown").

### Other crypto

p28 is the DRM stack (unstripped ARM ELF: libtomcrypt 1.16, ECDSA P-256, AES, SHA; Widevine, Marlin, HDCP,
secure storage). `tzfw` is TrustZone firmware. Kernel images p21/p22 are MTK "LZ4P"-wrapped; p20 is an LZHS blob.

### Recovery

Only from the UART console, which RELEASE locks out: `swum` (EPK from FAT32 USB), `swu` (EPK over TFTP),
`loadm`/`load` (raw image to a partition), `usbboot`, `partinfo`, `setboot`, `swumode on`. SWUE's USB update path
accepts signed EPKs only.

### What is not signature-checked at boot

lgres, lgfont, model, logo, cfginfo, pqldb, skype, tplist, preload, vocdb, and the ext4 data partitions.

### Debug level

This unit is **RELEASE (2)**. NVM byte `0x18a` (0 DEBUG, 1 EVENT, 2 RELEASE) is in an I²C EEPROM (LG NVM handle
ch 3 / dev 0xA8 → MTK SIF bus 0, clkdiv 380, 2-byte big-endian subaddress), not on the eMMC. U-Boot ignores it
when the valid mark at `0x14` is `ff ff ff ff` and treats values > 2 as RELEASE. DEBUG/EVENT allow UART break-in
and skip the eFuse burn on a signature failure, but don't auto-clear a bad snapshot. The level takes effect at the
next cold boot. SIF ioctl struct: `{bus, clkdiv, dev, u8 *subaddr, subaddr_len, u8 *buf, count, pad}`.
Tool: `src/tools/ncdebug.c` (`status`, `dump`, `set <level> [--yes]`, `toggle [--yes]`; dry run without `--yes`;
addressing self-test; one-byte write with read-back).

Open: whether the full-verify OTP is already set (`0xf0008664` bit0, readable with `devmem`), and whether the
MICOM power-cycles a hung SoC.

## Next steps

- On the TV: test Settings, Terminal, Remote Desktop and QML; confirm the media player and RetroArch visually; live-test nclite.
- Kernel modules: tun, cifs (check `input_dev`-style struct offsets against LG's code first, as for xpadone).
- Mute while the screen saver is dark (no extra procmgr registrations: those can't be unregistered).
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
| `src/kmod/` | out-of-tree kernel modules (nchello, xpadone) for the stock 3.0.13 kernel |
| `src/qml/hello/` | QML Hello World (static Qt 6) |
| `third_party/kernel/` | linux-3.0.13 (configured for vermagic) + Bootlin GCC 7.3 toolchain |
| `third_party/qt/` | Qt 6.12 sources, `build-host.sh`, `build-target.sh`, eglfs_netcast plugin (in the qtbase tree) |
| `third_party/freerdp-build/`, `lvgl/`, `libvterm-0.3.3/`, `retro/` | FreeRDP, LVGL, libvterm, RetroArch + cores |
| `tv_dump/lglib/` | TV libraries and binaries pulled for reverse engineering, plus disassembly (`*.S`) |
| `tv_dump/glibc/` | LG glibc 2.12.2 from the TV's `/lib` |
| `sdk_extract/` | carved SDK resource zip (deletable) |
