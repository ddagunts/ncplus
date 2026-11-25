// SPDX-License-Identifier: GPL-2.0-only
/*
 * KMS driver for OSD plane 3 of the MediaTek MT5398 TV SoC (LG NetCast), the plane that
 * feeds the 1920x1080 panel. It replaces simplefb: one CRTC, one primary plane, GEM DMA
 * buffers, page flips.
 *
 * The OSD takes no register write per frame. Each plane points (plane +4, address >> 4) at
 * a region header in DRAM that the OSD fetches every frame; plane +0x20.. mirrors the
 * header words it last fetched. Header words used here (LG GPL U-Boot, board/mt5398/module):
 *   w0  bits 31:28 colour mode (14 = ARGB8888), bit 27 next region enable
 *   w1  bits 23:0 bitmap address bits 27:4, bits 31:24 mix weight (region alpha)
 *   w2  bits 25:24 bitmap address bits 29:28
 *   w4  input width | height << 12 | (pitch / 16) << 23
 *   w7  bits 28:27 bitmap address bits 31:30, bits 31:30 mix select
 *       (0 none, 1 per-pixel alpha, 2 region alpha = w1's mix weight, 3 plane)
 * A flip rewrites the address fields, then strobes OSD +0 bits 0 and 3 (_OSD_UpdateReg).
 * Buffers come from a CMA pool inside
 * 0x10000000-0x1fffffff, so a flip between two of them changes only w1, in one write.
 *
 * nckexec (src/kmod) sets the plane up before the warm boot: native 1920x1080 ARGB8888,
 * scaler SC3 bypassed, header in a reserved page. The driver takes over only that state and
 * leaves the geometry, the scaler and the timing generator alone.
 *
 * DPMS: disabling the CRTC also switches the panel backlight off through the DT's
 * "backlight" device (the MICOM, lg-micom-bl), so screen blanking darkens the panel.
 *
 * No OSD vsync interrupt is known, so vblank is an hrtimer, locked to the raster: the
 * panel's timing generator (PSC, optional regs "raster") has a live line counter, so each tick
 * re-arms itself for the wanted line and follows the panel's real rate. LG's software leaves the
 * panel at 50 Hz (2200 x 1350) or 60 Hz (2200 x 1125), whichever it was using at the jump;
 * the mode reported follows. Without the raster regs (or if PSC isn't on the output path) it
 * is a plain 60 Hz timer as before.
 * A flip's event is held until the plane's mirror shows the OSD has fetched the new header
 * (at most 3 ticks). The OSD fetches at one line of the frame; the first flips find that
 * line by polling the mirror, then vblank is placed right after it, so a flip queued just
 * after the event always makes the next frame.
 */

#include <linux/backlight.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include <drm/drm_aperture.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_client_setup.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#define DRIVER_NAME	"mt5398-osd"

/* OSD block (reg 0): update strobe, plane 3, scaler 3 (read only here). */
#define OSD_UPDATE		0x000
#define OSD_UPDATE_NOW		BIT(0)
#define OSD_UPDATE_OSD1		BIT(3)
#define OSD_PLA3		0x300
#define OSD_PLA_CTRL		0x00
#define OSD_PLA_CTRL_EN		BIT(0)
#define OSD_PLA_HDR		0x04
#define OSD_PLA_BLEND		0x08
#define OSD_PLA_FETCHED(n)	(0x20 + 4 * (n))
#define OSD_SC3			0x600

/* Region header words. */
#define HDR_WORDS		8
#define HDR_W0_MODE		GENMASK(31, 28)
#define HDR_W0_NEXT_EN		BIT(27)
#define HDR_MODE_ARGB8888	14
#define HDR_W1_ADDR		GENMASK(23, 0)
#define HDR_W1_WEIGHT		GENMASK(31, 24)
#define HDR_W2_ADDR28		GENMASK(25, 24)
#define HDR_W2_ASEL		GENMASK(31, 30)
#define HDR_W7_ADDR30		GENMASK(28, 27)
#define HDR_W7_MIX		GENMASK(31, 30)
#define MIX_REGION		2

#define OSD_W			1920
#define OSD_H			1080
#define HDR_W4(pitch)		(OSD_W | (OSD_H << 12) | ((pitch) / 16) << 23)
#define PITCH_MAX		(511 * 16)

/* Flip latency cap when the mirror never matches: deliver the event anyway. */
#define FLIP_MAX_TICKS		3

/*
 * Panel raster (reg "raster" = PSC + 0x300; LG GPL U-Boot hw_psc.h): line counter and totals.
 * Read only. PSC is only on the output path if the route register's bit 0 is set (LG's
 * fgIsMJCToOSTG(), reg "route" = 0xf0027804): don't touch PSC otherwise.
 */
#define PSC_TOTAL		0x0c	/* PSCSYS_03: htotal << 16 | vtotal */
#define PSC_COUNT		0x80	/* PSCSYS_20: h counter << 16 | v counter */
#define ROUTE_PSC		BIT(0)
#define PANEL_DCLK_KHZ		148500	/* both of LG's modes; only for the nominal frame time */

/* Latch line learning: poll the mirror every LATCH_POLL_NS until the OSD has fetched the header. */
#define LATCH_SAMPLES		8
#define LATCH_POLL_NS		(100 * NSEC_PER_USEC)
#define LATCH_POLL_MAX		250	/* 25 ms */
#define LATCH_MARGIN_LINES	4	/* vblank this far after the learnt line */

static unsigned int xrgb_mix = MIX_REGION;
module_param(xrgb_mix, uint, 0444);
MODULE_PARM_DESC(xrgb_mix, "OSD mix select for XRGB8888 (0 none, 1 pixel, 2 region [default], 3 plane)");

struct mt5398_osd {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	void __iomem *regs;
	struct backlight_device *backlight;	/* optional */
	void __iomem *hdr;		/* plane 3's region header in DRAM (uncached) */
	u32 hdr_stock[HDR_WORDS];	/* as nckexec left it */
	bool mirror_ok;			/* plane +0x24 tracks the fetched w1 */

	struct hrtimer vblank_timer;
	ktime_t period;

	/* Panel raster, NULL if not available: fixed 60 Hz timer */
	void __iomem *raster;
	u32 htotal, vtotal;
	u32 frame_ns;			/* a frame in our clock's terms, measured */
	struct drm_display_mode mode;	/* what the panel is running */
	unsigned int target;		/* line the vblank tick aims at */
	struct hrtimer latch_timer;	/* while learning the latch line */
	u16 latch_line[LATCH_SAMPLES];
	unsigned int n_latch, latch_polls;
	u32 latch_w1;
	bool vblank_on;			/* under vblank_timer's own lock-free protocol */

	/* Under drm.event_lock. */
	struct drm_pending_vblank_event *event;
	u32 flip_w1;
	unsigned int flip_ticks;
	/* flip events sent: OSD fetched the new header (or no mirror) / gave up waiting */
	unsigned long n_flip_latched, n_flip_timeout;
};

static struct mt5398_osd *to_osd(struct drm_device *drm)
{
	return container_of(drm, struct mt5398_osd, drm);
}

/* CEA 1080p60: the panel's mode; only the totals matter here (vblank period). */
static const struct drm_display_mode osd_mode = {
	DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED, 148500,
		 1920, 2008, 2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC)
};

static const u32 osd_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const u64 osd_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

/* LG's _OSD_UpdateReg(): latch the shadowed plane and scaler registers. */
static void osd_update(struct mt5398_osd *osd)
{
	writel(readl(osd->regs + OSD_UPDATE) | OSD_UPDATE_NOW, osd->regs + OSD_UPDATE);
	writel(readl(osd->regs + OSD_UPDATE) | OSD_UPDATE_OSD1, osd->regs + OSD_UPDATE);
}

static void osd_plane_enable(struct mt5398_osd *osd, bool on)
{
	void __iomem *ctrl = osd->regs + OSD_PLA3 + OSD_PLA_CTRL;
	u32 v = readl(ctrl), nv = on ? v | OSD_PLA_CTRL_EN : v & ~OSD_PLA_CTRL_EN;

	if (nv == v)
		return;
	writel(nv, ctrl);
	osd_update(osd);
}

/*
 * Point the header at a new bitmap. Words that don't change aren't written, so a flip
 * between two pool buffers of the same format is a single store to w1, which the OSD
 * fetches whole. Returns the w1 that the mirror will show once the OSD has fetched it.
 */
static u32 osd_hdr_program(struct mt5398_osd *osd, dma_addr_t addr, u32 pitch, u32 format)
{
	u32 w[HDR_WORDS], n[HDR_WORDS], weight, mix;
	unsigned int i;

	for (i = 0; i < HDR_WORDS; i++)
		w[i] = n[i] = readl(osd->hdr + 4 * i);

	/*
	 * Measured on the TV (FINDINGS.md, boot 15): pixel mix with AlphaSel 3 gives correct
	 * per-pixel alpha, and region mix with weight 0xff is opaque.
	 */
	if (format == DRM_FORMAT_ARGB8888) {
		/* Whatever stock blended with (pixel mix): the console has always used that. */
		weight = FIELD_GET(HDR_W1_WEIGHT, osd->hdr_stock[1]);
		mix = FIELD_GET(HDR_W7_MIX, osd->hdr_stock[7]);
	} else {
		/* X channel is undefined: blend with a constant, opaque region alpha. */
		weight = 0xff;
		mix = xrgb_mix & 3;
	}

	n[0] = (n[0] & ~HDR_W0_MODE) | FIELD_PREP(HDR_W0_MODE, HDR_MODE_ARGB8888);
	n[1] = FIELD_PREP(HDR_W1_WEIGHT, weight) | FIELD_PREP(HDR_W1_ADDR, (u32)addr >> 4);
	/* AlphaSel = which byte holds alpha; stock's 3 is the top byte (ARGB8888's A). */
	n[2] = (n[2] & ~(HDR_W2_ADDR28 | HDR_W2_ASEL)) |
	       FIELD_PREP(HDR_W2_ADDR28, (u32)addr >> 28) |
	       FIELD_PREP(HDR_W2_ASEL, FIELD_GET(HDR_W2_ASEL, osd->hdr_stock[2]));
	n[4] = HDR_W4(pitch);
	n[7] = (n[7] & ~(HDR_W7_ADDR30 | HDR_W7_MIX)) |
	       FIELD_PREP(HDR_W7_ADDR30, (u32)addr >> 30) | FIELD_PREP(HDR_W7_MIX, mix);

	/* w1 last: everything else is in place by the time the address moves. */
	for (i = 0; i < HDR_WORDS; i++)
		if (i != 1 && n[i] != w[i])
			writel(n[i], osd->hdr + 4 * i);
	if (n[1] != w[1])
		writel(n[1], osd->hdr + 4);
	/*
	 * nckexec's switch was followed by an update strobe, and an in-place header edit
	 * without one is unproven: strobe, as LG's flips go through _OSD_UpdateReg() too.
	 */
	if (memcmp(n, w, sizeof(n))) {
		wmb();
		osd_update(osd);
	}
	return n[1];
}

/* Panel raster */

static unsigned int osd_line(struct mt5398_osd *osd)
{
	return (readl(osd->raster + PSC_COUNT) & 0xffff) % osd->vtotal;
}

/* Time from now until the raster reaches the target line, at least a quarter of a frame away
 * so that one tick can't fire twice. */
static u64 osd_ns_to_target(struct mt5398_osd *osd)
{
	unsigned int lines = (osd->target + osd->vtotal - osd_line(osd)) % osd->vtotal;

	if (lines < osd->vtotal / 4)
		lines += osd->vtotal;
	return div_u64((u64)lines * osd->frame_ns, osd->vtotal);
}

/* The OSD has just fetched a flip's header (mirror matched): remember the line it did. */
static enum hrtimer_restart osd_latch_poll(struct hrtimer *t)
{
	struct mt5398_osd *osd = container_of(t, struct mt5398_osd, latch_timer);
	unsigned long flags;
	bool again = true;

	spin_lock_irqsave(&osd->drm.event_lock, flags);
	if (readl(osd->regs + OSD_PLA3 + OSD_PLA_FETCHED(1)) == osd->latch_w1) {
		if (osd->n_latch < LATCH_SAMPLES) {
			osd->latch_line[osd->n_latch++] = osd_line(osd);
			if (osd->n_latch == LATCH_SAMPLES) {
				u16 a[LATCH_SAMPLES];
				unsigned int i, j;

				/* median, so one odd sample doesn't matter */
				memcpy(a, osd->latch_line, sizeof(a));
				for (i = 1; i < LATCH_SAMPLES; i++)
					for (j = i; j && a[j - 1] > a[j]; j--)
						swap(a[j - 1], a[j]);
				osd->target = (a[LATCH_SAMPLES / 2] + LATCH_MARGIN_LINES) %
					      osd->vtotal;
			}
		}
		again = false;
	} else if (++osd->latch_polls >= LATCH_POLL_MAX) {
		again = false;
	}
	spin_unlock_irqrestore(&osd->drm.event_lock, flags);
	if (!again)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(t, ns_to_ktime(LATCH_POLL_NS));
	return HRTIMER_RESTART;
}

/* Plane */

static int osd_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *cs = NULL;
	struct drm_framebuffer *fb = ps->fb;
	u32 offset;
	int ret;

	if (ps->crtc)
		cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	ret = drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, true);
	if (ret || !ps->visible)
		return ret;

	/* The header holds the pitch in 16-byte units (9 bits) and a 16-byte aligned address. */
	offset = fb->offsets[0] + (ps->src.y1 >> 16) * fb->pitches[0] +
		 (ps->src.x1 >> 16) * fb->format->cpp[0];
	if (fb->pitches[0] % 16 || fb->pitches[0] > PITCH_MAX || offset % 16) {
		drm_dbg_kms(plane->dev, "pitch %u / offset %u not usable by the OSD\n",
			    fb->pitches[0], offset);
		return -EINVAL;
	}
	return 0;
}

static void osd_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct mt5398_osd *osd = to_osd(plane->dev);
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	dma_addr_t addr;
	unsigned long flags;
	u32 w1;

	if (!ps->visible) {
		osd_plane_enable(osd, false);
		return;
	}

	addr = drm_fb_dma_get_gem_addr(ps->fb, ps, 0);
	w1 = osd_hdr_program(osd, addr, ps->fb->pitches[0], ps->fb->format->format);
	osd_plane_enable(osd, true);

	spin_lock_irqsave(&osd->drm.event_lock, flags);
	osd->flip_w1 = w1;
	if (osd->raster && osd->mirror_ok && osd->n_latch < LATCH_SAMPLES &&
	    readl(osd->regs + OSD_PLA3 + OSD_PLA_FETCHED(1)) != w1) {
		osd->latch_w1 = w1;
		osd->latch_polls = 0;
		hrtimer_start(&osd->latch_timer, ns_to_ktime(LATCH_POLL_NS), HRTIMER_MODE_REL);
	}
	spin_unlock_irqrestore(&osd->drm.event_lock, flags);
}

static void osd_plane_atomic_disable(struct drm_plane *plane, struct drm_atomic_state *state)
{
	osd_plane_enable(to_osd(plane->dev), false);
}

static const struct drm_plane_helper_funcs osd_plane_helper_funcs = {
	.atomic_check = osd_plane_atomic_check,
	.atomic_update = osd_plane_atomic_update,
	.atomic_disable = osd_plane_atomic_disable,
};

static const struct drm_plane_funcs osd_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

/* CRTC and vblank */

static enum hrtimer_restart osd_vblank_tick(struct hrtimer *t)
{
	struct mt5398_osd *osd = container_of(t, struct mt5398_osd, vblank_timer);
	unsigned long flags;

	if (!READ_ONCE(osd->vblank_on))
		return HRTIMER_NORESTART;

	drm_crtc_handle_vblank(&osd->crtc);

	spin_lock_irqsave(&osd->drm.event_lock, flags);
	if (osd->event &&
	    (!osd->mirror_ok ||
	     readl(osd->regs + OSD_PLA3 + OSD_PLA_FETCHED(1)) == osd->flip_w1 ||
	     ++osd->flip_ticks >= FLIP_MAX_TICKS)) {
		if (osd->mirror_ok && osd->flip_ticks >= FLIP_MAX_TICKS)
			osd->n_flip_timeout++;
		else
			osd->n_flip_latched++;
		drm_crtc_send_vblank_event(&osd->crtc, osd->event);
		osd->event = NULL;
		drm_crtc_vblank_put(&osd->crtc);
	}
	spin_unlock_irqrestore(&osd->drm.event_lock, flags);

	if (osd->raster) {
		/* to now + the time to the target line: hrtimer_forward() counts from the old expiry */
		ktime_t now = hrtimer_cb_get_time(t);

		hrtimer_forward(t, now, ktime_add_ns(ktime_sub(now, hrtimer_get_expires(t)),
						     osd_ns_to_target(osd)));
	} else {
		hrtimer_forward_now(t, osd->period);
	}
	return HRTIMER_RESTART;
}

static int osd_enable_vblank(struct drm_crtc *crtc)
{
	struct mt5398_osd *osd = to_osd(crtc->dev);

	WRITE_ONCE(osd->vblank_on, true);
	hrtimer_start(&osd->vblank_timer,
		      osd->raster ? ns_to_ktime(osd_ns_to_target(osd)) : osd->period,
		      HRTIMER_MODE_REL);
	return 0;
}

/* Called under the vblank locks the tick takes: never wait for a running tick here. */
static void osd_disable_vblank(struct drm_crtc *crtc)
{
	struct mt5398_osd *osd = to_osd(crtc->dev);

	WRITE_ONCE(osd->vblank_on, false);
	hrtimer_try_to_cancel(&osd->vblank_timer);
}

static enum drm_mode_status osd_crtc_mode_valid(struct drm_crtc *crtc,
						const struct drm_display_mode *mode)
{
	return mode->hdisplay == OSD_W && mode->vdisplay == OSD_H ? MODE_OK : MODE_BAD;
}

static void osd_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	drm_crtc_vblank_on(crtc);
	backlight_enable(to_osd(crtc->dev)->backlight);
}

static void osd_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct mt5398_osd *osd = to_osd(crtc->dev);

	backlight_disable(osd->backlight);
	osd_plane_enable(osd, false);

	spin_lock_irq(&crtc->dev->event_lock);
	if (osd->event) {
		drm_crtc_send_vblank_event(crtc, osd->event);
		osd->event = NULL;
		drm_crtc_vblank_put(crtc);
	}
	spin_unlock_irq(&crtc->dev->event_lock);

	drm_crtc_vblank_off(crtc);

	spin_lock_irq(&crtc->dev->event_lock);
	if (crtc->state->event && !crtc->state->active) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irq(&crtc->dev->event_lock);
}

static void osd_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct mt5398_osd *osd = to_osd(crtc->dev);
	struct drm_pending_vblank_event *event = crtc->state->event;
	unsigned long flags;

	if (!event)
		return;
	crtc->state->event = NULL;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (crtc->state->active && !osd->event && drm_crtc_vblank_get(crtc) == 0) {
		osd->event = event;
		osd->flip_ticks = 0;
	} else {
		drm_crtc_send_vblank_event(crtc, event);
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static const struct drm_crtc_helper_funcs osd_crtc_helper_funcs = {
	.mode_valid = osd_crtc_mode_valid,
	.atomic_check = drm_crtc_helper_atomic_check,
	.atomic_enable = osd_crtc_atomic_enable,
	.atomic_disable = osd_crtc_atomic_disable,
	.atomic_flush = osd_crtc_atomic_flush,
};

static const struct drm_crtc_funcs osd_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = osd_enable_vblank,
	.disable_vblank = osd_disable_vblank,
};

/* Connector: the panel, always there, one mode. */

static int osd_connector_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &to_osd(connector->dev)->mode);
}

static const struct drm_connector_helper_funcs osd_connector_helper_funcs = {
	.get_modes = osd_connector_get_modes,
};

static const struct drm_connector_funcs osd_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	/* no .destroy: drmm_connector_init() cleans up itself and refuses one (-EINVAL) */
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs osd_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Disables before enables, then planes on active CRTCs only: the CRTC owns the plane bit. */
static const struct drm_mode_config_helper_funcs osd_mode_config_helpers = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

DEFINE_DRM_GEM_DMA_FOPS(osd_fops);

static const struct drm_driver osd_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops = &osd_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name = DRIVER_NAME,
	.desc = "MediaTek MT5398 OSD",
	.date = "20261123",
	.major = 1,
	.minor = 0,
};

/* Find plane 3's header and check it is the 1080p layer nckexec hands over. */
static int osd_hw_init(struct mt5398_osd *osd, struct device *dev)
{
	void __iomem *pla = osd->regs + OSD_PLA3;
	struct reserved_mem *rmem;
	struct device_node *np;
	u32 ctrl, hdr, *w = osd->hdr_stock;
	unsigned int i;

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	rmem = np ? of_reserved_mem_lookup(np) : NULL;
	of_node_put(np);
	if (!rmem)
		return dev_err_probe(dev, -ENODEV, "no region header page (memory-region 0)\n");

	ctrl = readl(pla + OSD_PLA_CTRL);
	hdr = (readl(pla + OSD_PLA_HDR) & GENMASK(27, 0)) << 4;
	if (!(ctrl & OSD_PLA_CTRL_EN) || hdr < rmem->base ||
	    hdr + HDR_WORDS * 4 > rmem->base + rmem->size)
		return dev_err_probe(dev, -ENODEV,
				     "plane 3 ctrl 0x%08x header 0x%08x not in the reserved page %pa\n",
				     ctrl, hdr, &rmem->base);

	osd->hdr = devm_ioremap(dev, hdr, HDR_WORDS * 4);
	if (!osd->hdr)
		return -ENOMEM;
	for (i = 0; i < HDR_WORDS; i++)
		w[i] = readl(osd->hdr + 4 * i);

	dev_info(dev, "plane 3 header 0x%08x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		 hdr, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
	dev_info(dev, "plane 3 ctrl %08x blend %08x\n", ctrl, readl(pla + OSD_PLA_BLEND));
	dev_info(dev, "fetched w0..w4 %08x %08x %08x %08x %08x, SC3 %08x src %08x\n",
		 readl(pla + OSD_PLA_FETCHED(0)), readl(pla + OSD_PLA_FETCHED(1)),
		 readl(pla + OSD_PLA_FETCHED(2)), readl(pla + OSD_PLA_FETCHED(3)),
		 readl(pla + OSD_PLA_FETCHED(4)), readl(osd->regs + OSD_SC3),
		 readl(osd->regs + OSD_SC3 + 4));

	if (FIELD_GET(HDR_W0_MODE, w[0]) != HDR_MODE_ARGB8888 || (w[0] & HDR_W0_NEXT_EN) ||
	    w[4] != HDR_W4(OSD_W * 4))
		return dev_err_probe(dev, -ENODEV,
				     "plane 3 is not the native 1080p ARGB layer nckexec sets up\n");

	osd->mirror_ok = readl(pla + OSD_PLA_FETCHED(1)) == w[1];
	if (!osd->mirror_ok)
		dev_warn(dev, "fetched-header mirror doesn't match: flip events go by the timer\n");
	return 0;
}

static int osd_kms_init(struct mt5398_osd *osd)
{
	struct drm_device *drm = &osd->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = OSD_W;
	drm->mode_config.max_width = OSD_W;
	drm->mode_config.min_height = OSD_H;
	drm->mode_config.max_height = OSD_H;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &osd_mode_config_funcs;
	drm->mode_config.helper_private = &osd_mode_config_helpers;

	ret = drm_universal_plane_init(drm, &osd->plane, 0, &osd_plane_funcs, osd_formats,
				       ARRAY_SIZE(osd_formats), osd_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&osd->plane, &osd_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &osd->crtc, &osd->plane, NULL, &osd_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&osd->crtc, &osd_crtc_helper_funcs);

	ret = drm_simple_encoder_init(drm, &osd->encoder, DRM_MODE_ENCODER_NONE);
	if (ret)
		return ret;
	osd->encoder.possible_crtcs = drm_crtc_mask(&osd->crtc);

	ret = drmm_connector_init(drm, &osd->connector, &osd_connector_funcs,
				  DRM_MODE_CONNECTOR_LVDS, NULL);
	if (ret)
		return ret;
	drm_connector_helper_add(&osd->connector, &osd_connector_helper_funcs);
	ret = drm_connector_attach_encoder(&osd->connector, &osd->encoder);
	if (ret)
		return ret;

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	return 0;
}

static void osd_timer_fini(struct drm_device *drm, void *arg)
{
	struct mt5398_osd *osd = arg;

	WRITE_ONCE(osd->vblank_on, false);
	hrtimer_cancel(&osd->vblank_timer);
	hrtimer_cancel(&osd->latch_timer);
}

/*
 * Panel raster: the mode the timing generator is running, which LG's software chose. Only reads.
 * Any doubt (no regs, PSC not on the path, counter not moving, odd totals): stay on the fixed
 * 60 Hz timer.
 */
static void osd_raster_init(struct mt5398_osd *osd, struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res, *rres;
	void __iomem *raster, *route;
	u32 total, v, c0, c1, l0, l1, nominal;
	u64 ns, frames;
	ktime_t t0, t1;

	osd->mode = osd_mode;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "raster");
	rres = platform_get_resource_byname(pdev, IORESOURCE_MEM, "route");
	if (!res || !rres)
		return;
	route = devm_ioremap(dev, rres->start, resource_size(rres));
	if (!route || !(readl(route) & ROUTE_PSC)) {
		dev_info(dev, "PSC not on the output path: fixed 60 Hz timer\n");
		return;
	}
	raster = devm_ioremap(dev, res->start, resource_size(res));
	if (!raster)
		return;
	total = readl(raster + PSC_TOTAL);
	v = total & 0xffff;
	if (v < OSD_H + 1 || v > 2200 || (total >> 16) < OSD_W || (total >> 16) > 4400) {
		dev_warn(dev, "panel total %08x not plausible: fixed 60 Hz timer\n", total);
		return;
	}
	c0 = readl(raster + PSC_COUNT);
	usleep_range(100, 200);
	c1 = readl(raster + PSC_COUNT);
	if ((c0 & 0xffff) == (c1 & 0xffff)) {
		dev_warn(dev, "panel line counter not moving (%08x): fixed 60 Hz timer\n", c0);
		return;
	}
	osd->raster = raster;
	osd->htotal = total >> 16;
	osd->vtotal = v;
	osd->target = 0;

	/*
	 * The panel's clock and ours differ by 0.3% or so: time some frames with the counter.
	 * n whole frames plus the line difference; the nominal length only fixes n.
	 */
	nominal = div_u64((u64)osd->htotal * osd->vtotal * 1000000, PANEL_DCLK_KHZ);
	t0 = ktime_get();
	l0 = osd_line(osd);
	msleep(100);
	t1 = ktime_get();
	l1 = osd_line(osd);
	ns = ktime_to_ns(ktime_sub(t1, t0));
	l1 = (l1 + osd->vtotal - l0) % osd->vtotal;
	frames = div_u64(ns - div_u64((u64)l1 * nominal, osd->vtotal) + nominal / 2, nominal);
	osd->frame_ns = div64_u64(ns * osd->vtotal, frames * osd->vtotal + l1);
	if (abs((s64)osd->frame_ns - nominal) > nominal / 50) {
		dev_warn(dev, "frame time %u ns, nominal %u: using nominal\n", osd->frame_ns, nominal);
		osd->frame_ns = nominal;
	}

	osd->mode.htotal = osd->htotal;
	osd->mode.vtotal = osd->vtotal;
	osd->mode.hsync_start = OSD_W + (osd->htotal - OSD_W) / 4;
	osd->mode.hsync_end = osd->mode.hsync_start + (osd->htotal - OSD_W) / 8;
	osd->mode.vsync_start = OSD_H + 4;
	osd->mode.vsync_end = OSD_H + 9;
	drm_mode_set_name(&osd->mode);
	dev_info(dev, "panel raster %u x %u, %u Hz mode, frame %u.%03u us (%u nominal)\n",
		 osd->htotal, osd->vtotal, drm_mode_vrefresh(&osd->mode), osd->frame_ns / 1000,
		 osd->frame_ns % 1000, nominal / 1000);
}

static int osd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct backlight_device *bl;
	struct mt5398_osd *osd;
	u64 frame_ns;
	int ret;

	/* First, so a deferral (backlight not registered yet) leaves the hardware untouched. */
	bl = devm_of_find_backlight(dev);
	if (IS_ERR(bl))
		return dev_err_probe(dev, PTR_ERR(bl), "backlight\n");

	osd = devm_drm_dev_alloc(dev, &osd_driver, struct mt5398_osd, drm);
	if (IS_ERR(osd))
		return PTR_ERR(osd);
	osd->backlight = bl;
	platform_set_drvdata(pdev, osd);

	osd->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(osd->regs))
		return PTR_ERR(osd->regs);

	ret = osd_hw_init(osd, dev);
	if (ret)
		return ret;

	/* Scanout buffers: the CMA pool in memory-region 1 (bank 0, one 256 MB window). */
	ret = of_reserved_mem_device_init_by_idx(dev, dev->of_node, 1);
	if (ret)
		dev_warn(dev, "no scanout pool (%d): flips may take two header writes\n", ret);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	osd_raster_init(osd, pdev);
	frame_ns = div_u64((u64)osd->mode.htotal * osd->mode.vtotal * 1000000, osd->mode.clock);
	osd->period = ns_to_ktime(frame_ns);
	hrtimer_init(&osd->vblank_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	osd->vblank_timer.function = osd_vblank_tick;
	hrtimer_init(&osd->latch_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	osd->latch_timer.function = osd_latch_poll;
	ret = drmm_add_action_or_reset(&osd->drm, osd_timer_fini, osd);
	if (ret)
		return ret;

	ret = osd_kms_init(osd);
	if (ret)
		return ret;

	/* simplefb on the same plane: unregister it, fbcon moves to our fbdev. */
	ret = drm_aperture_remove_framebuffers(&osd_driver);
	if (ret)
		return ret;

	ret = drm_dev_register(&osd->drm, 0);
	if (ret)
		return ret;

	/* ARGB8888 for the console: fbcon's palette is then opaque, as it was on simplefb. */
	drm_client_setup_with_fourcc(&osd->drm, DRM_FORMAT_ARGB8888);
	return 0;
}

static void osd_remove(struct platform_device *pdev)
{
	struct mt5398_osd *osd = platform_get_drvdata(pdev);

	drm_dev_unplug(&osd->drm);
	drm_atomic_helper_shutdown(&osd->drm);
	of_reserved_mem_device_release(&pdev->dev);
}

static void osd_shutdown(struct platform_device *pdev)
{
	struct mt5398_osd *osd = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(&osd->drm);
}

/*
 * "<latched> <timeout> blend .. hdr ..": how flip events were completed (see
 * osd_vblank_tick), the plane's +8 register and the live header words.
 */
static ssize_t flip_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt5398_osd *osd = dev_get_drvdata(dev);
	unsigned long flags, latched, timeout;

	spin_lock_irqsave(&osd->drm.event_lock, flags);
	latched = osd->n_flip_latched;
	timeout = osd->n_flip_timeout;
	spin_unlock_irqrestore(&osd->drm.event_lock, flags);
	return sysfs_emit(buf, "%lu %lu blend %08x hdr w1 %08x w2 %08x w7 %08x%s raster %ux%u tick at line %u, %u latch samples\n",
			  latched, timeout, readl(osd->regs + OSD_PLA3 + OSD_PLA_BLEND),
			  readl(osd->hdr + 4), readl(osd->hdr + 8), readl(osd->hdr + 28),
			  osd->mirror_ok ? "" : " (no mirror: all by timer)",
			  osd->htotal, osd->vtotal, osd->target, osd->n_latch);
}
static DEVICE_ATTR_RO(flip_stats);

static struct attribute *osd_attrs[] = {
	&dev_attr_flip_stats.attr,
	NULL
};
ATTRIBUTE_GROUPS(osd);

static const struct of_device_id osd_of_match[] = {
	{ .compatible = "mediatek,mt5398-osd" },
	{ }
};
MODULE_DEVICE_TABLE(of, osd_of_match);

static struct platform_driver osd_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = osd_of_match,
		.dev_groups = osd_groups,
	},
	.probe = osd_probe,
	.remove = osd_remove,
	.shutdown = osd_shutdown,
};
module_platform_driver(osd_platform_driver);

MODULE_DESCRIPTION("MediaTek MT5398 OSD plane 3 KMS driver");
MODULE_LICENSE("GPL");
