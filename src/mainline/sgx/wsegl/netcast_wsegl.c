// SPDX-License-Identifier: MIT
/*
 * A window-system module (WSEGL, interface version 2) for LG's PowerVR DDK 1.9 EGL
 * (libIMGegl.so) on the mainline kernel: pbuffers, and windows that are a set of buffers the
 * caller owns (netcast_window.h), each wrapped for the GPU with PVRSRVWrapExtMemory(). Nothing
 * here touches the display: the caller's swap() shows a finished buffer.
 *
 * LG's IMGegl first tries libpvrPVR2D_CobraNetCast40WSEGL.so (needs LG's libUMA.so, absent
 * under Debian) and then falls back to libpvrEWS_NullWSEGL.so, so this is installed under
 * that name. LG's own libpvrPVR2D_NullWSEGL.so rejects every display.
 *
 * The interface (written from how LG's two WSEGL modules behave, not from IMG headers):
 * WSEGL_GetFunctionTablePointer() returns { version 2, 14 functions }; errors are
 * 0 success, 1 cannot initialise, 2 bad display, 3 bad window, 4 bad pixmap, 6 bad
 * drawable; caps are { type, value } pairs ending in type 0; configs are 8 words ending
 * in drawable type 0.
 */
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "netcast_window.h"

typedef unsigned long u32;

enum {
	WSEGL_SUCCESS = 0,
	WSEGL_CANNOT_INITIALISE = 1,
	WSEGL_BAD_NATIVE_DISPLAY = 2,
	WSEGL_BAD_NATIVE_WINDOW = 3,
	WSEGL_BAD_NATIVE_PIXMAP = 4,
	WSEGL_BAD_DRAWABLE = 6,
};

enum {
	WSEGL_NO_CAPS = 0,
	WSEGL_CAP_MIN_SWAP_INTERVAL = 1,
	WSEGL_CAP_MAX_SWAP_INTERVAL = 2,
	WSEGL_CAP_WINDOWS_USE_HW_SYNC = 3,	/* as in LG's module: we wait on the sync object */
};
enum { WSEGL_NO_DRAWABLE = 0, WSEGL_DRAWABLE_WINDOW = 1, WSEGL_DRAWABLE_PIXMAP = 2 };
enum { WSEGL_PIXELFORMAT_565 = 0, WSEGL_PIXELFORMAT_8888 = 2 };

struct wsegl_caps {
	u32 type;
	u32 value;
};

struct wsegl_config {
	u32 drawable_type;
	u32 pixel_format;
	u32 native_renderable;
	u32 framebuffer_level;
	u32 native_visual_id;
	void *native_visual;
	u32 transparent_type;
	u32 transparent_colour;
};

static const struct wsegl_caps caps[] = {
	{ WSEGL_CAP_WINDOWS_USE_HW_SYNC, 1 },
	{ WSEGL_CAP_MIN_SWAP_INTERVAL, 0 },
	{ WSEGL_CAP_MAX_SWAP_INTERVAL, 1 },
	{ WSEGL_NO_CAPS, 0 },
};

/* window configs (EGL adds pbuffer support to them) */
static struct wsegl_config configs[] = {
	{ WSEGL_DRAWABLE_WINDOW, WSEGL_PIXELFORMAT_8888, 0, 0, 0, NULL, 0, 0 },
	{ WSEGL_DRAWABLE_WINDOW, WSEGL_PIXELFORMAT_565, 0, 0, 0, NULL, 0, 0 },
	{ WSEGL_NO_DRAWABLE, 0, 0, 0, 0, NULL, 0, 0 },
};

static int display_cookie;

static u32 is_display_valid(void *native)
{
	(void)native;
	return WSEGL_SUCCESS;
}

static u32 initialise_display(void *native, void **handle, const struct wsegl_caps **c,
			      struct wsegl_config **cfg)
{
	(void)native;
	*handle = &display_cookie;
	*c = caps;
	*cfg = configs;
	return WSEGL_SUCCESS;
}

static u32 close_display(void *handle)
{
	(void)handle;
	return WSEGL_SUCCESS;
}

/* --- PowerVR services (libsrv_um.so, DDK 1.9 public header services.h) ---------------- */

struct dev_id {
	u32 type, cls, index;
	char *p1, *p2;
};

struct sync_info {			/* PVRSRV_CLIENT_SYNC_INFO, 24 bytes */
	void *data;
	u32 w[5];
};

struct mem_info {			/* PVRSRV_CLIENT_MEM_INFO, 44 bytes */
	void *lin, *lin_km;
	u32 dev_vaddr, flags, client_flags, size;
	struct sync_info *sync;
	void *mapping, *kernel, *res, *next;
};

#define PVRSRV_OK 0
#define PVRSRV_ERROR_RETRY 24

static struct {
	u32 (*Connect)(void **, u32);
	u32 (*EnumerateDevices)(void *, u32 *, struct dev_id *);
	u32 (*AcquireDeviceData)(void *, u32, void *, u32);
	u32 (*CreateDeviceMemContext)(void *, void **, u32 *, void *);
	u32 (*WrapExtMemory)(void *, void *, u32, u32, u32, void *, void *, u32, struct mem_info **);
	u32 (*UnwrapExtMemory)(void *, struct mem_info *);
	u32 (*SyncOpsTakeToken)(void *, struct sync_info *, void *);
	u32 (*SyncOpsFlushToToken)(void *, struct sync_info *, const void *, u32);
} srv;

static struct {
	int state;			/* 0 not tried, 1 ready, -1 failed */
	void *conn, *mem_context;
	unsigned char devdata[512];	/* PVRSRV_DEV_DATA: opaque to us */
} pvr;

static int debug_on(void)
{
	return getenv("NETCAST_WSEGL_DEBUG") != NULL;
}

static int pvr_init(void)
{
	static unsigned char heaps[4096] __attribute__((aligned(8)));
	struct dev_id ids[32];
	void *lib;
	u32 n = 0, heapn = 0, i;

	if (pvr.state)
		return pvr.state > 0;
	pvr.state = -1;
	lib = dlopen("libsrv_um.so", RTLD_NOW | RTLD_GLOBAL);
	if (!lib)
		return 0;
#define S(f) (*(void **)&srv.f = dlsym(lib, "PVRSRV" #f)); if (!srv.f) return 0
	S(Connect); S(EnumerateDevices); S(AcquireDeviceData); S(CreateDeviceMemContext);
	S(WrapExtMemory); S(UnwrapExtMemory); S(SyncOpsTakeToken); S(SyncOpsFlushToToken);
#undef S
	if (srv.Connect(&pvr.conn, 0) != PVRSRV_OK)
		return 0;
	memset(ids, 0, sizeof(ids));
	if (srv.EnumerateDevices(pvr.conn, &n, ids) != PVRSRV_OK)
		return 0;
	for (i = 0; i < n && i < 32; i++)
		if (ids[i].cls == 0)		/* PVRSRV_DEVICE_CLASS_3D */
			break;
	if (i >= n || i >= 32 || srv.AcquireDeviceData(pvr.conn, ids[i].index, pvr.devdata, 0) != PVRSRV_OK)
		return 0;
	if (srv.CreateDeviceMemContext(pvr.devdata, &pvr.mem_context, &heapn, heaps) != PVRSRV_OK)
		return 0;
	pvr.state = 1;
	return 1;
}

/* --- window drawables ------------------------------------------------------------------ */

struct drawable {
	struct netcast_window *win;
	struct mem_info *mem[NETCAST_WINDOW_MAX_BUFFERS];
	unsigned int cur;		/* being rendered */
	u32 swaps;
};

struct params {				/* 11 words; 7..10 left zero */
	u32 width, height, stride, format;
	void *lin;
	u32 dev_vaddr;
	void *mem;
	u32 w7, w8, w9, w10;
};

static void unwrap_all(struct drawable *d)
{
	unsigned int i;

	for (i = 0; i < NETCAST_WINDOW_MAX_BUFFERS; i++)
		if (d->mem[i]) {
			srv.UnwrapExtMemory(pvr.devdata, d->mem[i]);
			d->mem[i] = NULL;
		}
}

static u32 create_window(void *dpy, struct wsegl_config *c, void **dr, void *w, u32 *rot)
{
	struct netcast_window *win = w;
	struct drawable *d;
	unsigned int i;
	u32 err;

	(void)dpy; (void)c;
	if (!win || ((unsigned long)win & 3) || win->magic != NETCAST_WINDOW_MAGIC ||
	    win->nbuffers < 2 || win->nbuffers > NETCAST_WINDOW_MAX_BUFFERS || !win->swap ||
	    win->stride % 32 || win->stride < win->width ||
	    win->size < win->stride * win->height * 4)
		return WSEGL_BAD_NATIVE_WINDOW;
	if (!pvr_init())
		return WSEGL_CANNOT_INITIALISE;
	d = calloc(1, sizeof(*d));
	if (!d)
		return WSEGL_CANNOT_INITIALISE;
	d->win = win;
	for (i = 0; i < win->nbuffers; i++) {
		err = srv.WrapExtMemory(pvr.devdata, pvr.mem_context, win->size, 0, 0, NULL,
					win->map[i], 0, &d->mem[i]);
		if (err != PVRSRV_OK || !d->mem[i] || !d->mem[i]->sync) {
			if (debug_on())
				fprintf(stderr, "netcast-wsegl: wrap buffer %u failed (%lu)\n", i, err);
			unwrap_all(d);
			free(d);
			return WSEGL_BAD_NATIVE_WINDOW;
		}
		if (debug_on())
			fprintf(stderr, "netcast-wsegl: buffer %u cpu %p gpu %08lx size %lx sync %p data %p\n",
				i, win->map[i], d->mem[i]->dev_vaddr, d->mem[i]->size,
				(void *)d->mem[i]->sync, d->mem[i]->sync->data);
	}
	*dr = d;
	*rot = 0;
	return WSEGL_SUCCESS;
}

static u32 delete_drawable(void *dr)
{
	struct drawable *d = dr;

	if (!d || !d->win)
		return WSEGL_BAD_DRAWABLE;
	unwrap_all(d);
	free(d);
	return WSEGL_SUCCESS;
}

static void fill_params(struct drawable *d, unsigned int n, struct params *p)
{
	memset(p, 0, sizeof(*p));
	p->width = d->win->width;
	p->height = d->win->height;
	p->stride = d->win->stride;
	p->format = WSEGL_PIXELFORMAT_8888;
	p->lin = d->win->map[n];
	p->dev_vaddr = d->mem[n]->dev_vaddr;
	p->mem = d->mem[n];
}

/* source: the last shown buffer, render: the one being drawn */
static u32 get_drawable_parameters(void *dr, void *src, void *rnd)
{
	struct drawable *d = dr;

	if (!d || !d->win)
		return WSEGL_BAD_DRAWABLE;
	fill_params(d, d->cur, rnd);
	fill_params(d, (d->cur + d->win->nbuffers - 1) % d->win->nbuffers, src);
	return WSEGL_SUCCESS;
}

/* Wait for the GPU to finish with the buffer (the render is already queued); 2 s cap. */
static int wait_gpu(struct sync_info *sync)
{
	u32 token[8] = { 0 }, err;
	struct timespec t0, t;

	if (srv.SyncOpsTakeToken(pvr.conn, sync, token) != PVRSRV_OK)
		return 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		err = srv.SyncOpsFlushToToken(pvr.conn, sync, token, 0);
		if (err != PVRSRV_ERROR_RETRY)
			break;
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (t.tv_sec - t0.tv_sec > 2)
			break;
		usleep(200);
	}
	return err == PVRSRV_OK;
}

static u32 swap_drawable(void *dr, u32 v)
{
	struct drawable *d = dr;
	unsigned int n;

	(void)v;
	if (!d || !d->win)
		return WSEGL_BAD_DRAWABLE;
	n = d->cur;
	if (!d->win->no_wait && !wait_gpu(d->mem[n]->sync) && debug_on())
		fprintf(stderr, "netcast-wsegl: GPU wait failed on buffer %u\n", n);
	d->win->swap(d->win->priv, n);
	d->cur = (n + 1) % d->win->nbuffers;
	d->swaps++;
	return WSEGL_SUCCESS;
}

static u32 create_pixmap(void *d, struct wsegl_config *c, void **dr, void *p, u32 *rot)
{
	(void)d; (void)c; (void)dr; (void)p; (void)rot;
	return WSEGL_BAD_NATIVE_PIXMAP;
}

static u32 ok2(void *dr, u32 v)
{
	(void)dr; (void)v;
	return WSEGL_SUCCESS;
}

static u32 copy_from_drawable(void *dr, void *pixmap)
{
	(void)dr; (void)pixmap;
	return WSEGL_BAD_NATIVE_PIXMAP;
}

static u32 copy_from_pbuffer(void *addr, u32 w, u32 h, u32 stride, u32 fmt, void *pixmap)
{
	(void)addr; (void)w; (void)h; (void)stride; (void)fmt; (void)pixmap;
	return WSEGL_BAD_NATIVE_PIXMAP;
}

static u32 ok_drawable(void *dr)
{
	(void)dr;
	return WSEGL_SUCCESS;
}

static const struct {
	u32 version;
	void *fn[14];
} table = {
	2,
	{
		(void *)is_display_valid,
		(void *)initialise_display,
		(void *)close_display,
		(void *)create_window,
		(void *)create_pixmap,
		(void *)delete_drawable,
		(void *)swap_drawable,
		(void *)ok2,			/* swap control interval */
		(void *)ok2,			/* wait native */
		(void *)copy_from_drawable,
		(void *)copy_from_pbuffer,
		(void *)get_drawable_parameters,
		(void *)ok_drawable,		/* connect drawable */
		(void *)ok_drawable,		/* disconnect drawable */
	},
};

const void *WSEGL_GetFunctionTablePointer(void)
{
	return &table;
}
