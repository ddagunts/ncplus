# Cross-build libnc and apps for the LG NetCast TV.
#   make            build everything into src/out/
#   make deploy     upload out/ to <stick>/netcast/apps on the TV (via temporary ftpd, tools/push.sh)
TOP      := $(abspath ..)
SDK      := $(TOP)/toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot
CROSS    := $(SDK)/bin/arm-lgtv-linux-gnueabi-
SYSROOT  := $(SDK)/arm-lgtv-linux-gnueabi/sysroot
CC       := $(CROSS)gcc
AR       := $(CROSS)ar
OUT      := out
STUBS    := $(OUT)/stubs

CFLAGS   := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation \
            -I libnc -I $(TOP)/third_party/khronos
LDFLAGS  := -static-libgcc -L $(STUBS) -Wl,--allow-shlib-undefined
# EGL/GLES2 resolve to the TV's /mnt/lg/res/lglib/lib{EGL,GLESv2}.so at runtime (no SONAME, so NEEDED is
# the plain file name). At link time we use stubs that export the same symbols (stubs/*.syms, taken from
# the TV's libraries), so no LG files are needed to build.
LIBS     := -lEGL -lGLESv2 -ldl -lpthread -lm -lrt

APPS     := $(OUT)/ncmusic $(OUT)/ncsmb $(OUT)/smbgw $(OUT)/ncterm $(OUT)/ncsettings $(OUT)/ncplay $(OUT)/ncinput $(OUT)/nctext $(OUT)/ncres $(OUT)/sdldemo $(OUT)/launcher $(OUT)/photos $(OUT)/nclite $(OUT)/ncscreen $(OUT)/ncnosleep $(OUT)/peek $(OUT)/sniff $(OUT)/pktlog $(OUT)/wlq $(OUT)/ncnetkeep

# Developer tools (packaged into netcast/tools/ by build/package.sh unless --no-dev)
DEVTOOLS := $(OUT)/nckey $(OUT)/evdump $(OUT)/ncdebug

all: $(APPS) $(DEVTOOLS) $(OUT)/run-app.sh

$(STUBS)/.ok: stubs/libEGL.syms stubs/libGLESv2.syms
	mkdir -p $(STUBS)
	for l in libEGL libGLESv2; do \
	  sed 's/.*/void &(void) {}/' stubs/$$l.syms > $(STUBS)/$$l.c && \
	  $(CC) -shared -nostdlib -fPIC $(STUBS)/$$l.c -o $(STUBS)/$$l.so || exit 1; \
	done
	touch $@

$(OUT)/libnc.a: libnc/nc.c libnc/ncgfx.c libnc/nc.h libnc/ncgfx.h libnc/keynames.h | $(STUBS)/.ok
	$(CC) $(CFLAGS) -c libnc/nc.c -o $(OUT)/nc.o
	$(CC) $(CFLAGS) -c libnc/ncgfx.c -o $(OUT)/ncgfx.o
	$(AR) rcs $@ $(OUT)/nc.o $(OUT)/ncgfx.o

$(OUT)/ncinput: apps/ncinput.c $(OUT)/libnc.a
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(OUT)/libnc.a $(LIBS)

# FreeType linked statically from the buildroot SDK (the TV's own libfreetype is older).
FT_CFLAGS := -I $(SYSROOT)/usr/include/freetype2
FT_LIBS   := $(SYSROOT)/usr/lib/libfreetype.a $(SYSROOT)/usr/lib/libz.a $(SYSROOT)/usr/lib/libbz2.a
$(OUT)/nctext: apps/nctext.c libnc/nctext.c libnc/nctext.h $(OUT)/libnc.a
	$(CC) $(CFLAGS) $(FT_CFLAGS) apps/nctext.c libnc/nctext.c -o $@ $(LDFLAGS) $(OUT)/libnc.a $(FT_LIBS) $(LIBS)

$(OUT)/launcher: apps/launcher.c libnc/nctext.c libnc/nctext.h $(OUT)/libnc.a
	$(CC) $(CFLAGS) $(FT_CFLAGS) apps/launcher.c libnc/nctext.c -o $@ $(LDFLAGS) $(OUT)/libnc.a $(FT_LIBS) $(LIBS)

$(OUT)/photos: apps/photos.c libnc/nctext.c libnc/nctext.h $(OUT)/libnc.a
	$(CC) $(CFLAGS) -Wno-sign-compare $(FT_CFLAGS) -I $(TOP)/third_party/stb apps/photos.c libnc/nctext.c -o $@ $(LDFLAGS) $(OUT)/libnc.a $(FT_LIBS) $(LIBS)

$(OUT)/ncplay: apps/ncplay.c libnc/nctext.c libnc/nctext.h $(OUT)/libnc.a
	$(CC) $(CFLAGS) $(FT_CFLAGS) apps/ncplay.c libnc/nctext.c -o $@ $(LDFLAGS) $(OUT)/libnc.a $(FT_LIBS) $(LIBS)

$(OUT)/nclite: apps/nclite.c $(OUT)/libnc.a
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(OUT)/libnc.a $(LIBS)

$(OUT)/ncnosleep: tools/ncnosleep.c
	$(CC) -O2 -Wall -Wno-format-truncation -static $< -o $@

$(OUT)/ncdebug: tools/ncdebug.c
	$(CC) -O2 -Wall -Wextra -static $< -o $@

$(OUT)/evdump: tools/evdump.c
	$(CC) -O2 -Wall -static $< -o $@

$(OUT)/nckey: tools/nckey.c
	$(CC) -O2 -Wall -Wextra -static $< -o $@

$(OUT)/peek: tools/peek.c
	$(CC) -O2 -static $< -o $@

$(OUT)/sniff: tools/sniff.c
	$(CC) -O2 -static $< -o $@

$(OUT)/pktlog: tools/pktlog.c
	$(CC) -O2 -Wall -Wextra -static $< -o $@

$(OUT)/wlq: tools/wlq.c
	$(CC) -O2 -Wall -Wextra -static $< -o $@

$(OUT)/ncnetkeep: tools/ncnetkeep.c
	$(CC) -O2 -Wall -Wextra -static $< -o $@

$(OUT)/ncscreen: apps/ncscreen.c
	$(CC) -O2 -Wall -static $< -o $@

$(OUT)/ncres: apps/ncres.c $(OUT)/libnc.a
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(OUT)/libnc.a $(LIBS)

# SDL2 2.32.10 with the "netcast" video driver (third_party/sdl2-netcast, built by build-sdl2.sh)
SDL2DIR  := $(TOP)/third_party/sdl2-netcast
$(OUT)/sdldemo: apps/sdldemo.c $(SDL2DIR)/lib/libSDL2.a
	$(CC) -O2 -g -Wall -I $(SDL2DIR)/include/SDL2 $< -o $@ -static-libgcc $(SDL2DIR)/lib/libSDL2.a -ldl -lpthread -lm -lrt

# Music (dr_libs FLAC/MP3/WAV) and Network Shares (libsmb2) on SDL2 + SDL2_ttf
SMB2DIR := $(TOP)/third_party/smb2-netcast
SDLUI_CFLAGS := -O2 -g -Wall -I $(SDL2DIR)/include/SDL2 -I apps -D_REENTRANT
SDLUI_LIBS := -static-libgcc $(SDL2DIR)/lib/libSDL2_ttf.a $(SDL2DIR)/lib/libSDL2.a $(FT_LIBS) -ldl -lpthread -lm -lrt
$(OUT)/ncmusic: apps/ncmusic.c apps/sdlui.c apps/sdlui.h $(SDL2DIR)/lib/libSDL2.a
	$(CC) $(SDLUI_CFLAGS) -I $(TOP)/third_party/dr_libs -I $(TOP)/third_party/stb apps/ncmusic.c apps/sdlui.c -o $@ $(SDLUI_LIBS)
$(OUT)/ncsmb: apps/ncsmb.c apps/sdlui.c apps/smbcommon.c apps/sdlui.h apps/smbcommon.h $(SMB2DIR)/lib/libsmb2.a
	$(CC) $(SDLUI_CFLAGS) -I $(SMB2DIR)/include apps/ncsmb.c apps/sdlui.c apps/smbcommon.c -o $@ $(SMB2DIR)/lib/libsmb2.a $(SDLUI_LIBS)
$(OUT)/smbgw: apps/smbgw.c apps/smbcommon.c apps/smbcommon.h $(SMB2DIR)/lib/libsmb2.a
	$(CC) -O2 -g -Wall -I apps -I $(SMB2DIR)/include apps/smbgw.c apps/smbcommon.c -o $@ -static-libgcc $(SMB2DIR)/lib/libsmb2.a -lpthread

# libvterm 0.3.3 (static) + ncterm
VTERM_DIR := $(TOP)/third_party/libvterm-0.3.3
VTERM_OBJS := $(patsubst $(VTERM_DIR)/src/%.c,$(OUT)/vterm/%.o,$(wildcard $(VTERM_DIR)/src/*.c))
$(OUT)/vterm/%.o: $(VTERM_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -std=c99 -I $(VTERM_DIR)/include -c $< -o $@
$(OUT)/libvterm.a: $(VTERM_OBJS)
	$(AR) rcs $@ $^
$(OUT)/ncterm: apps/ncterm.c libnc/nctext.c libnc/nctext.h $(OUT)/libnc.a $(OUT)/libvterm.a
	$(CC) $(CFLAGS) $(FT_CFLAGS) -I $(VTERM_DIR)/include apps/ncterm.c libnc/nctext.c -o $@ $(LDFLAGS) $(OUT)/libnc.a $(OUT)/libvterm.a $(FT_LIBS) $(LIBS) -lutil

# LVGL 9.6 (static) on the SDL2 netcast driver
LVGL_DIR  := $(TOP)/third_party/lvgl
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OUT)/lvgl/%.o,$(LVGL_SRCS))
LVGL_CFLAGS := -O2 -I $(LVGL_DIR) -I lvgl -DLV_CONF_INCLUDE_SIMPLE -I $(SDL2DIR)/include/SDL2 -D_REENTRANT
$(OUT)/lvgl/%.o: $(LVGL_DIR)/%.c lvgl/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(LVGL_CFLAGS) -c $< -o $@
$(OUT)/liblvgl.a: $(LVGL_OBJS)
	$(AR) rcs $@ $^
$(OUT)/ncsettings: apps/ncsettings.c $(OUT)/liblvgl.a $(SDL2DIR)/lib/libSDL2.a
	$(CC) $(LVGL_CFLAGS) -Wall $< -o $@ -static-libgcc $(OUT)/liblvgl.a $(SDL2DIR)/lib/libSDL2.a -ldl -lpthread -lm -lrt

$(OUT)/run-app.sh: apps/run-app.sh
	cp $< $@ && chmod +x $@

check: all
	@for f in $(APPS); do printf '%s: ' $$f; $(CROSS)readelf -V $$f | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1; done

deploy: all
	$(TOP)/tools/push.sh apps $(APPS) $(OUT)/run-app.sh

clean:
	rm -rf $(OUT)

.PHONY: all check deploy clean
