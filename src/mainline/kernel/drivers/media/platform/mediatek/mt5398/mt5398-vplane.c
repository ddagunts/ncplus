// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT5398 video plane (LG NetCast TVs): a V4L2 video output device that shows frames on
 * the panel's video plane, under the OSD, with no CPU conversion.
 *
 * Path: B2R ("block to raster", 0xf0026000) reads a picture in the video decoder's own layout
 * from DRAM and feeds the main video path: PDS/regen (0xf0023000), deinterlacer (0xf0024000),
 * scaler (0xf0025000), MJC frame-rate converter (0xf0036000), output stage and mixer
 * (0xf0027000), then the panel timing (0xf005e000), where the OSD planes are mixed on top.
 * Layout: luma in 64x32 tiles, then interleaved CbCr in 64x16 tiles, row-major, height padded
 * to 32 lines (what mt5398-vdec writes, its V4L2_PIX_FMT_MT5398_TILED).
 *
 * Nothing here comes from LG code. LG's stock software leaves the path set up for whatever
 * input it last showed (usually the muted analog tuner). The driver sets it up for a 1280x720
 * B2R source at the first stream start: the B2R clock and channel registers as LG's player
 * leaves them for 720p (their meanings from the order in which LG's _VDP_Hal* functions write
 * them), the path words from mt5398-vplane-path.h (read back from the hardware while LG's
 * player showed 720p), then the path's import triggers, which latch the new sizes (as LG's
 * video resolution manager fires them after a mode change). Without the MJC words or the
 * triggers the panel stays black.
 *
 * Formats on the OUTPUT queue (multi-planar API, one plane):
 *   NV12   any size up to 1280x720 (even); the CPU tiles each frame into one of the driver's
 *          scanout buffers, centred on black, and the buffer is returned at once. vmalloc
 *          buffers, so DMABUFs from mt5398-vdec's NV12 CAPTURE queue import with no copy.
 *   MT64   the tiled layout, 1280x720 only; the buffer itself is scanned out (dma-contig, from
 *          the DT's memory-region) and returned when the next one is on screen.
 * Flips happen on B2R's vsync interrupt (+0x30c bit 4); B2R runs 720p at 30 Hz (LG's timing for
 * a 30 fps clip), so a queue faster than that shows the latest frame each vsync.
 *
 * While streaming, OSD plane 3 (the desktop, opaque) is switched off through mt5398_osd, so
 * the video fills the panel; hide_osd=0 keeps it on.
 *
 * Not done: other source sizes (the scaler ratios and B2R timing for them), B2R rates other
 * than 30 Hz, a window inside the desktop, interlaced sources, audio/video sync beyond
 * "show the latest frame at the next vsync".
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mt5398-osd.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "mt5398-vplane-path.h"

#define DRV_NAME "mt5398-vplane"

#define V4L2_PIX_FMT_MT5398_TILED v4l2_fourcc('M', 'T', '6', '4')

/* the one B2R mode set up here: 1280x720 */
#define VP_W		1280
#define VP_H		720
#define VP_LUMA		(VP_W * ALIGN(VP_H, 32))		/* 64x32 tiles */
#define VP_SIZE		(VP_LUMA * 3 / 2)
#define NSCAN		4	/* NV12: shown, the one before it (still latching), pending, writing */

/* B2R, HD channel at +0x000 (SD at +0x100 is left off) */
#define B2R_Y		0x000
#define B2R_C		0x004
#define B2R_SIZE	0x01c	/* width / 2 | height << 20 */
#define B2R_TIMING	0x020	/* htotal * 2 - 1 | (vtotal - 1) << 16 */
#define B2R_PITCH	0x028	/* bytes / 16 */
#define B2R_UPDATE	0x030	/* bit 0, set after enabling */
#define B2R_STATUS	0x03c	/* line counter bits 27:16 */
#define B2R_ENABLE	0x040	/* 0x11, then | 0x04 */
#define B2R_RV_Y	0x050	/* right view (3D); LG keeps it equal to the left in 2D */
#define B2R_RV_C	0x054
#define B2R_SHARED	0x300
#define B2R_IRQ		0x30c	/* W1C: bit 4 HD vsync, bit 5 SD vsync */
#define B2R_IRQ_HD	BIT(4)
#define B2R_IRQ_SD	BIT(5)

/* CKGEN words LG's player sets with B2R (+0x228 bit 0 = B2R clock on, last) */
static const u32 ckgen_720p30[][2] = {
	{ 0x22c, 0x016c007d },	/* B2R clock divider */
	{ 0x378, 0x80001d55 },
	{ 0x394, 0x00000002 },
};
#define CKGEN_B2R_ON	0x228

/* the HD channel's mode words for 1280x720 at 30 Hz, as LG's player leaves them */
static const u32 b2r_720p30[][2] = {
	{ 0x008, 0x00000808 }, { 0x00c, 0 }, { 0x010, 0 }, { 0x014, 0 },
	{ 0x018, 0x00108080 },
	{ B2R_SIZE, (VP_W / 2) | VP_H << 20 },
	{ B2R_TIMING, 0x02ed0ce3 },	/* 1650 x 750 */
	{ 0x024, 0x00020005 },
	{ B2R_PITCH, VP_W / 16 },
	{ 0x02c, 0x00a08403 },
	{ 0x034, 0x0080087f }, { 0x038, 0 },
	{ 0x044, 0x001f001f }, { 0x048, 0 }, { 0x04c, 0 },
	{ 0x058, 0x0000ff00 }, { 0x05c, 0 },
};
#define B2R_SHARED_720P	0x00010089

/* the main path's import triggers (bits set, except the scaler's, which is pulsed) */
#define PDS_IMPORT	0xf0023164	/* bit 4 */
#define REGEN_IMPORT	0xf00231b0	/* bit 25 */
#define DI_SETRES	0xf0024010	/* bit 27 */
#define SCALER_IMPORT	0xf0025228	/* bit 1: 1 = import off, back to 0 */

static bool hide_osd = true;
module_param(hide_osd, bool, 0644);
MODULE_PARM_DESC(hide_osd, "switch the OSD (desktop) off while a video is streaming [default on]");

/* path register windows, in the DT's reg order after "b2r" */
enum { W_CKGEN, W_PATH, W_POST, W_TIMING, W_MJC, W_NUM };
static const char *const win_names[W_NUM] = { "ckgen", "path", "post", "timing", "mjc" };

struct vplane_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct vplane_scan {
	void *va;
	dma_addr_t dma;
};

/* what is on screen, or about to be: a scanout buffer (NV12) or a queue buffer (MT64) */
struct vplane_slot {
	int scan;			/* NV12: scanout index, or -1 */
	struct vplane_buf *buf;		/* MT64, or NULL */
};

struct vplane {
	struct device *dev;
	struct v4l2_device v4l2;
	struct video_device vdev;
	struct mutex lock;		/* vdev and queue */
	struct vb2_queue queue;
	struct v4l2_pix_format_mplane fmt;

	void __iomem *b2r;
	struct { phys_addr_t phys; resource_size_t size; void __iomem *va; } win[W_NUM];
	bool hw_ready;			/* the path has been set up for 720p */

	struct vplane_scan scan[NSCAN];
	struct work_struct tile_work;

	/* Under slock. */
	spinlock_t slock;
	bool running;
	struct list_head queued;
	struct vplane_slot pending, shown, prev;
	unsigned int scan_busy;		/* NV12: bit per scanout buffer being written */
	u64 n_irq, n_flip, n_drop, n_tiled, tile_ns;

	struct dentry *dbg;
};

static inline struct vplane_buf *to_vbuf(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct vplane_buf, vb);
}

/* B2R's clock: off after a cold boot (LG only turns it on to play), and B2R isn't read then */
static bool b2r_clocked(const struct vplane *vp)
{
	return readl(vp->win[W_CKGEN].va + CKGEN_B2R_ON) & 1;
}

static bool native_fmt(const struct vplane *vp)
{
	return vp->fmt.pixelformat == V4L2_PIX_FMT_MT5398_TILED;
}

/* ---- registers ---- */

static void __iomem *path_reg(struct vplane *vp, u32 phys)
{
	int i;

	for (i = 0; i < W_NUM; i++)
		if (phys >= vp->win[i].phys && phys + 4 <= vp->win[i].phys + vp->win[i].size)
			return vp->win[i].va + (phys - vp->win[i].phys);
	return NULL;
}

static void path_set_bits(struct vplane *vp, u32 phys, u32 set, u32 clr)
{
	void __iomem *r = path_reg(vp, phys);

	writel((readl(r) & ~clr) | set, r);
}

static void b2r_show(struct vplane *vp, dma_addr_t luma)
{
	writel(luma, vp->b2r + B2R_Y);
	writel(luma + VP_LUMA, vp->b2r + B2R_C);
	writel(luma, vp->b2r + B2R_RV_Y);
	writel(luma + VP_LUMA, vp->b2r + B2R_RV_C);
}

/* B2R clock and 720p30 mode, the path table, the import triggers; B2R shows @luma. */
static void vplane_hw_setup(struct vplane *vp, dma_addr_t luma)
{
	void __iomem *ck = vp->win[W_CKGEN].va;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ckgen_720p30); i++)
		writel(ckgen_720p30[i][1], ck + ckgen_720p30[i][0]);
	writel(readl(ck + CKGEN_B2R_ON) | 1, ck + CKGEN_B2R_ON);

	writel(0, vp->b2r + B2R_ENABLE);
	b2r_show(vp, luma);
	for (i = 0; i < ARRAY_SIZE(b2r_720p30); i++)
		writel(b2r_720p30[i][1], vp->b2r + b2r_720p30[i][0]);
	writel(B2R_SHARED_720P, vp->b2r + B2R_SHARED);
	writel(0x11, vp->b2r + B2R_ENABLE);
	writel(0x15, vp->b2r + B2R_ENABLE);
	writel(readl(vp->b2r + B2R_UPDATE) | 1, vp->b2r + B2R_UPDATE);

	for (i = 0; i < ARRAY_SIZE(vplane_path_720p); i++)
		writel(vplane_path_720p[i][1], path_reg(vp, vplane_path_720p[i][0]));

	path_set_bits(vp, PDS_IMPORT, BIT(4), 0);
	path_set_bits(vp, REGEN_IMPORT, BIT(25), 0);
	path_set_bits(vp, DI_SETRES, BIT(27), 0);
	path_set_bits(vp, SCALER_IMPORT, BIT(1), 0);
	path_set_bits(vp, SCALER_IMPORT, 0, BIT(1));

	dev_info(vp->dev, "video path set up for %ux%u (%zu path words)\n", VP_W, VP_H,
		 ARRAY_SIZE(vplane_path_720p));
}

/* ---- NV12 -> tiled ---- */

static void fill_black(void *dst)
{
	memset(dst, 0x10, VP_LUMA);
	memset(dst + VP_LUMA, 0x80, VP_LUMA / 2);
}

/*
 * One NV12 frame (w x h, bytes per line bpl, chroma right after the luma) into a scanout
 * buffer at (ox, oy): ox a multiple of 64, oy even. Only the picture's area is written.
 */
static void tile_nv12(void *dst, const u8 *src, u32 bpl, u32 w, u32 h, u32 ox, u32 oy)
{
	const u8 *csrc = src + bpl * h;
	u8 *cdst = dst + VP_LUMA;
	u32 x, y;

	for (y = 0; y < h; y++) {
		u32 dy = oy + y;
		u8 *row = dst + (dy / 32) * (VP_W * 32) + (dy % 32) * 64 + (ox / 64) * 2048;
		const u8 *s = src + y * bpl;

		for (x = 0; x < w; x += 64, row += 2048)
			memcpy(row, s + x, min(64u, w - x));
	}
	for (y = 0; y < h / 2; y++) {
		u32 dy = oy / 2 + y;
		u8 *row = cdst + (dy / 16) * (VP_W * 16) + (dy % 16) * 64 + (ox / 64) * 1024;
		const u8 *s = csrc + y * bpl;

		for (x = 0; x < w; x += 64, row += 1024)
			memcpy(row, s + x, min(64u, w - x));
	}
}

/* ---- flips ---- */

/* Under slock: give a slot back (NV12: the scanout buffer is free again). */
static void slot_release(struct vplane *vp, struct vplane_slot *s, enum vb2_buffer_state st)
{
	if (s->buf)
		vb2_buffer_done(&s->buf->vb.vb2_buf, st);
	s->buf = NULL;
	s->scan = -1;
}

static bool slot_empty(const struct vplane_slot *s)
{
	return s->scan < 0 && !s->buf;
}

/* NV12: a scanout buffer that is not on screen, latching, pending or being written. */
static int scan_free(struct vplane *vp)
{
	int i;

	for (i = 0; i < NSCAN; i++)
		if (i != vp->shown.scan && i != vp->prev.scan && i != vp->pending.scan &&
		    !(vp->scan_busy & BIT(i)))
			return i;
	return -1;
}

static void tile_work(struct work_struct *w)
{
	struct vplane *vp = container_of(w, struct vplane, tile_work);
	const struct v4l2_pix_format_mplane *f = &vp->fmt;
	u32 ox = ((VP_W - f->width) / 2) & ~63u, oy = ((VP_H - f->height) / 2) & ~1u;
	struct vplane_buf *buf;
	unsigned long flags;
	ktime_t t0;
	int i;

	for (;;) {
		spin_lock_irqsave(&vp->slock, flags);
		buf = list_first_entry_or_null(&vp->queued, struct vplane_buf, list);
		i = buf && vp->running ? scan_free(vp) : -1;
		if (i < 0) {
			spin_unlock_irqrestore(&vp->slock, flags);
			return;
		}
		list_del(&buf->list);
		vp->scan_busy |= BIT(i);
		spin_unlock_irqrestore(&vp->slock, flags);

		t0 = ktime_get();
		tile_nv12(vp->scan[i].va, vb2_plane_vaddr(&buf->vb.vb2_buf, 0),
			  f->plane_fmt[0].bytesperline, f->width, f->height, ox, oy);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

		spin_lock_irqsave(&vp->slock, flags);
		vp->scan_busy &= ~BIT(i);
		vp->tile_ns += ktime_to_ns(ktime_sub(ktime_get(), t0));
		vp->n_tiled++;
		if (vp->pending.scan >= 0)
			vp->n_drop++;		/* never shown: the newer frame wins */
		vp->pending.scan = i;
		spin_unlock_irqrestore(&vp->slock, flags);
	}
}

static irqreturn_t vplane_irq(int irq, void *data)
{
	struct vplane *vp = data;
	u32 st = readl(vp->b2r + B2R_IRQ) & (B2R_IRQ_HD | B2R_IRQ_SD);
	bool more = false;

	if (!st)
		return IRQ_NONE;
	writel(st, vp->b2r + B2R_IRQ);
	if (!(st & B2R_IRQ_HD))
		return IRQ_HANDLED;

	spin_lock(&vp->slock);
	vp->n_irq++;
	if (vp->running) {
		struct vplane_slot next = { .scan = -1 };

		/* the one replaced at the last vsync has been read for a whole frame now */
		slot_release(vp, &vp->prev, VB2_BUF_STATE_DONE);
		if (native_fmt(vp)) {
			next.buf = list_first_entry_or_null(&vp->queued, struct vplane_buf, list);
			if (next.buf)
				list_del(&next.buf->list);
		} else {
			next = vp->pending;
			vp->pending.scan = -1;
		}
		if (!slot_empty(&next)) {
			b2r_show(vp, next.buf ? vb2_dma_contig_plane_dma_addr(&next.buf->vb.vb2_buf, 0)
					      : vp->scan[next.scan].dma);
			vp->prev = vp->shown;
			vp->shown = next;
			vp->n_flip++;
			more = !native_fmt(vp);
		}
	}
	spin_unlock(&vp->slock);
	if (more)
		queue_work(system_highpri_wq, &vp->tile_work);	/* a scanout buffer may be free */
	return IRQ_HANDLED;
}

/* ---- vb2 ---- */

static int vp_queue_setup(struct vb2_queue *q, unsigned int *nbufs, unsigned int *nplanes,
			  unsigned int sizes[], struct device *alloc_devs[])
{
	struct vplane *vp = vb2_get_drv_priv(q);
	u32 size = vp->fmt.plane_fmt[0].sizeimage;

	if (*nplanes)
		return *nplanes != 1 || sizes[0] < size ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int vp_buf_prepare(struct vb2_buffer *vb)
{
	struct vplane *vp = vb2_get_drv_priv(vb->vb2_queue);
	u32 size = vp->fmt.plane_fmt[0].sizeimage;

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	if (!vb2_get_plane_payload(vb, 0))
		vb2_set_plane_payload(vb, 0, size);
	if (!native_fmt(vp) && !vb2_plane_vaddr(vb, 0))
		return -EFAULT;		/* an imported DMABUF that can't be mapped */
	return 0;
}

static void vp_buf_queue(struct vb2_buffer *vb)
{
	struct vplane *vp = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long flags;

	spin_lock_irqsave(&vp->slock, flags);
	list_add_tail(&to_vbuf(vb)->list, &vp->queued);
	spin_unlock_irqrestore(&vp->slock, flags);
	if (!native_fmt(vp))
		queue_work(system_highpri_wq, &vp->tile_work);
}

static int vp_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vplane *vp = vb2_get_drv_priv(q);
	unsigned long flags;
	int i;

	for (i = 0; i < NSCAN; i++)
		fill_black(vp->scan[i].va);
	if (!vp->hw_ready) {
		vplane_hw_setup(vp, vp->scan[0].dma);
		vp->hw_ready = true;
	}

	spin_lock_irqsave(&vp->slock, flags);
	vp->pending.scan = vp->prev.scan = -1;
	vp->shown.scan = 0;		/* black */
	vp->running = true;
	spin_unlock_irqrestore(&vp->slock, flags);
	b2r_show(vp, vp->scan[0].dma);

	if (hide_osd)
		mt5398_osd_video_fullscreen(true);
	if (!native_fmt(vp))
		queue_work(system_highpri_wq, &vp->tile_work);
	return 0;
}

static void vp_stop_streaming(struct vb2_queue *q)
{
	struct vplane *vp = vb2_get_drv_priv(q);
	struct vplane_buf *buf, *tmp;
	unsigned long flags;

	mt5398_osd_video_fullscreen(false);

	spin_lock_irqsave(&vp->slock, flags);
	vp->running = false;
	spin_unlock_irqrestore(&vp->slock, flags);
	cancel_work_sync(&vp->tile_work);

	/* black on screen before any buffer goes back; B2R keeps running */
	fill_black(vp->scan[0].va);
	b2r_show(vp, vp->scan[0].dma);
	msleep(70);			/* two vsyncs at 30 Hz */

	spin_lock_irqsave(&vp->slock, flags);
	slot_release(vp, &vp->pending, VB2_BUF_STATE_ERROR);
	slot_release(vp, &vp->shown, VB2_BUF_STATE_ERROR);
	slot_release(vp, &vp->prev, VB2_BUF_STATE_ERROR);
	list_for_each_entry_safe(buf, tmp, &vp->queued, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&vp->slock, flags);
}

static const struct vb2_ops vp_qops = {
	.queue_setup = vp_queue_setup,
	.buf_prepare = vp_buf_prepare,
	.buf_queue = vp_buf_queue,
	.start_streaming = vp_start_streaming,
	.stop_streaming = vp_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

/* ---- V4L2 ---- */

static void fill_fmt(struct v4l2_pix_format_mplane *f, u32 fourcc, u32 w, u32 h)
{
	memset(f->reserved, 0, sizeof(f->reserved));
	f->pixelformat = fourcc;
	f->num_planes = 1;
	f->field = V4L2_FIELD_NONE;
	/* the path converts with its own (LG's 720p) matrix: any colorimetry is taken as given */
	if (f->colorspace == V4L2_COLORSPACE_DEFAULT)
		f->colorspace = V4L2_COLORSPACE_REC709;
	memset(f->plane_fmt[0].reserved, 0, sizeof(f->plane_fmt[0].reserved));
	if (fourcc == V4L2_PIX_FMT_MT5398_TILED) {
		f->width = VP_W;
		f->height = VP_H;
		f->plane_fmt[0].bytesperline = VP_W;
		f->plane_fmt[0].sizeimage = VP_SIZE;
	} else {
		f->width = clamp_t(u32, ALIGN(w, 2), 64, VP_W);
		f->height = clamp_t(u32, ALIGN(h, 2), 64, VP_H);
		f->plane_fmt[0].bytesperline = f->width;
		f->plane_fmt[0].sizeimage = f->width * f->height * 3 / 2;
	}
}

static int vp_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "MT5398 video plane", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" DRV_NAME, sizeof(cap->bus_info));
	return 0;
}

static int vp_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	static const u32 fmts[] = { V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_MT5398_TILED };

	if (f->index >= ARRAY_SIZE(fmts))
		return -EINVAL;
	f->pixelformat = fmts[f->index];
	if (fmts[f->index] == V4L2_PIX_FMT_MT5398_TILED)
		strscpy(f->description, "MT5398 decoder tiles 64x32", sizeof(f->description));
	return 0;
}

static int vp_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vplane *vp = video_drvdata(file);

	f->fmt.pix_mp = vp->fmt;
	return 0;
}

static int vp_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;

	fill_fmt(p, p->pixelformat == V4L2_PIX_FMT_MT5398_TILED ? p->pixelformat
								 : V4L2_PIX_FMT_NV12,
		 p->width, p->height);
	return 0;
}

static int vp_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vplane *vp = video_drvdata(file);

	if (vb2_is_busy(&vp->queue))
		return -EBUSY;
	vp_try_fmt(file, priv, f);
	vp->fmt = f->fmt.pix_mp;
	/* NV12 is read by the CPU (cached vmalloc, DMABUFs vmapped), MT64 by B2R */
	vp->queue.mem_ops = native_fmt(vp) ? &vb2_dma_contig_memops : &vb2_vmalloc_memops;
	return 0;
}

static int vp_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fs)
{
	if (fs->index)
		return -EINVAL;
	if (fs->pixel_format == V4L2_PIX_FMT_MT5398_TILED) {
		fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fs->discrete.width = VP_W;
		fs->discrete.height = VP_H;
		return 0;
	}
	if (fs->pixel_format != V4L2_PIX_FMT_NV12)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise.min_width = 64;
	fs->stepwise.max_width = VP_W;
	fs->stepwise.step_width = 2;
	fs->stepwise.min_height = 64;
	fs->stepwise.max_height = VP_H;
	fs->stepwise.step_height = 2;
	return 0;
}

static int vp_enum_output(struct file *file, void *priv, struct v4l2_output *out)
{
	if (out->index)
		return -EINVAL;
	strscpy(out->name, "Panel", sizeof(out->name));
	out->type = V4L2_OUTPUT_TYPE_ANALOG;
	return 0;
}

static int vp_g_output(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int vp_s_output(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops vp_ioctl_ops = {
	.vidioc_querycap = vp_querycap,
	.vidioc_enum_fmt_vid_out = vp_enum_fmt,
	.vidioc_g_fmt_vid_out_mplane = vp_g_fmt,
	.vidioc_try_fmt_vid_out_mplane = vp_try_fmt,
	.vidioc_s_fmt_vid_out_mplane = vp_s_fmt,
	.vidioc_enum_framesizes = vp_enum_framesizes,
	.vidioc_enum_output = vp_enum_output,
	.vidioc_g_output = vp_g_output,
	.vidioc_s_output = vp_s_output,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations vp_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* ---- debugfs ---- */

static int stats_show(struct seq_file *m, void *unused)
{
	struct vplane *vp = m->private;
	unsigned long flags;
	u64 irq, flip, drop, tiled, ns;
	int i;

	spin_lock_irqsave(&vp->slock, flags);
	irq = vp->n_irq;
	flip = vp->n_flip;
	drop = vp->n_drop;
	tiled = vp->n_tiled;
	ns = vp->tile_ns;
	spin_unlock_irqrestore(&vp->slock, flags);
	seq_printf(m, "vsyncs %llu flips %llu tiled %llu dropped %llu tile-us-avg %llu\n",
		   irq, flip, tiled, drop, tiled ? div64_u64(ns, tiled * 1000) : 0);
	seq_puts(m, "scanout");
	for (i = 0; i < NSCAN; i++)
		seq_printf(m, " %pad", &vp->scan[i].dma);
	seq_puts(m, "\n");
	if (!b2r_clocked(vp))
		seq_puts(m, "b2r clock off\n");
	else
		seq_printf(m, "b2r y %08x c %08x status %08x enable %08x, path %s\n",
			   readl(vp->b2r + B2R_Y), readl(vp->b2r + B2R_C),
			   readl(vp->b2r + B2R_STATUS), readl(vp->b2r + B2R_ENABLE),
			   vp->hw_ready ? "set up" : "as left by LG");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

/* ---- probe ---- */

/* devm, so it runs after the dmam scanout buffers are freed */
static void vplane_release_mem(void *dev)
{
	of_reserved_mem_device_release(dev);
}

static int vplane_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct video_device *vdev;
	struct vplane *vp;
	struct resource *res;
	unsigned int i;
	int irq, ret;

	vp = devm_kzalloc(dev, sizeof(*vp), GFP_KERNEL);
	if (!vp)
		return -ENOMEM;
	vp->dev = dev;
	mutex_init(&vp->lock);
	spin_lock_init(&vp->slock);
	INIT_LIST_HEAD(&vp->queued);
	INIT_WORK(&vp->tile_work, tile_work);
	vp->pending.scan = vp->shown.scan = vp->prev.scan = -1;

	vp->b2r = devm_platform_ioremap_resource_byname(pdev, "b2r");
	if (IS_ERR(vp->b2r))
		return PTR_ERR(vp->b2r);
	/* shared with other drivers (CKGEN, the OSD's raster and route words): not requested */
	for (i = 0; i < W_NUM; i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, win_names[i]);
		if (!res)
			return dev_err_probe(dev, -EINVAL, "no \"%s\" registers\n", win_names[i]);
		vp->win[i].phys = res->start;
		vp->win[i].size = resource_size(res);
		vp->win[i].va = devm_ioremap(dev, res->start, resource_size(res));
		if (!vp->win[i].va)
			return -ENOMEM;
	}
	for (i = 0; i < ARRAY_SIZE(vplane_path_720p); i++)
		if (!path_reg(vp, vplane_path_720p[i][0]))
			return dev_err_probe(dev, -EINVAL, "path word %08x outside the DT's regs\n",
					     vplane_path_720p[i][0]);
	if (!path_reg(vp, PDS_IMPORT) || !path_reg(vp, REGEN_IMPORT) || !path_reg(vp, DI_SETRES) ||
	    !path_reg(vp, SCALER_IMPORT) || vp->win[W_CKGEN].size < 0x398)
		return dev_err_probe(dev, -EINVAL, "trigger or clock words outside the DT's regs\n");

	ret = of_reserved_mem_device_init(dev);
	if (ret)
		return dev_err_probe(dev, ret, "memory-region\n");
	ret = devm_add_action_or_reset(dev, vplane_release_mem, dev);
	if (ret)
		return ret;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	for (i = 0; i < NSCAN; i++) {
		vp->scan[i].va = dmam_alloc_attrs(dev, VP_SIZE, &vp->scan[i].dma, GFP_KERNEL,
						  DMA_ATTR_WRITE_COMBINE);
		if (!vp->scan[i].va)
			return -ENOMEM;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, vplane_irq, 0, DRV_NAME, vp);
	if (ret)
		return ret;

	fill_fmt(&vp->fmt, V4L2_PIX_FMT_NV12, VP_W, VP_H);
	vp->queue.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	vp->queue.io_modes = VB2_MMAP | VB2_DMABUF;
	vp->queue.drv_priv = vp;
	vp->queue.buf_struct_size = sizeof(struct vplane_buf);
	vp->queue.ops = &vp_qops;
	vp->queue.mem_ops = &vb2_vmalloc_memops;
	vp->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	vp->queue.lock = &vp->lock;
	vp->queue.dev = dev;
	ret = vb2_queue_init(&vp->queue);
	if (ret)
		return ret;

	ret = v4l2_device_register(dev, &vp->v4l2);
	if (ret)
		return ret;
	vdev = &vp->vdev;
	strscpy(vdev->name, DRV_NAME, sizeof(vdev->name));
	vdev->fops = &vp_fops;
	vdev->ioctl_ops = &vp_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &vp->lock;
	vdev->queue = &vp->queue;
	vdev->v4l2_dev = &vp->v4l2;
	vdev->vfl_dir = VFL_DIR_TX;
	vdev->device_caps = V4L2_CAP_VIDEO_OUTPUT_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, vp);
	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_device_unregister(&vp->v4l2);
		return ret;
	}

	platform_set_drvdata(pdev, vp);
	vp->dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("stats", 0444, vp->dbg, vp, &stats_fops);
	dev_info(dev, "%s, B2R clock %s\n", video_device_node_name(vdev),
		 b2r_clocked(vp) ? "on (LG played a video before the jump)" : "off");
	return 0;
}

static void vplane_remove(struct platform_device *pdev)
{
	struct vplane *vp = platform_get_drvdata(pdev);

	debugfs_remove_recursive(vp->dbg);
	video_unregister_device(&vp->vdev);
	v4l2_device_unregister(&vp->v4l2);
	/* B2R would keep reading the scanout buffers, which go with the device */
	if (vp->hw_ready)
		writel(0, vp->b2r + B2R_ENABLE);
}

static const struct of_device_id vplane_of_match[] = {
	{ .compatible = "mediatek,mt5398-vplane" },
	{ }
};
MODULE_DEVICE_TABLE(of, vplane_of_match);

static struct platform_driver vplane_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = vplane_of_match,
	},
	.probe = vplane_probe,
	.remove = vplane_remove,
};
module_platform_driver(vplane_driver);

MODULE_DESCRIPTION("MediaTek MT5398 video plane (B2R) V4L2 output");
MODULE_LICENSE("GPL");
