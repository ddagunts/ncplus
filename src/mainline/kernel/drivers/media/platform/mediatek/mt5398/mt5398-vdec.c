// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT5398 H.264 decoder (LG NetCast TVs): V4L2 stateless, frame-based, Annex B.
 *
 * The decoder has no firmware: the CPU programs every picture. The register meanings come
 * from tracing LG's stock driver while it played clips (src/kmod/ncvdec.c) and from the
 * order in which its code builds the values; no LG code is used. One hardware run per picture:
 *
 *   reset, bitstream FIFO = the OUTPUT buffer, barrel shifter at the first slice's start code,
 *   read the slice header through the shifter (it removes emulation prevention bytes) up to
 *   num_ref_idx_active, program the default reference lists, POCs and the SPS/PPS/slice words,
 *   let the hardware parse ref_pic_list_modification() and pred_weight_table() itself (it
 *   applies the modifications to the lists given), read the rest of the header, program the
 *   picture, start, wait. The hardware finds and parses the picture's further slices itself.
 *
 * Output: the hardware writes luma in 64x32 tiles and interleaved CbCr in 64x16 tiles,
 * row-major, height padded to 32 lines (V4L2_PIX_FMT_MT5398_TILED below, dma-contig). With
 * NV12 on the CAPTURE queue the driver decodes into its own tiled buffer per CAPTURE buffer
 * (references stay tiled) and untiles with the CPU into cached (vmalloc) CAPTURE buffers.
 *
 * Memory: the decoder only reaches DRAM bank 0 (the DT gives it a CMA pool there), and LG's
 * DRAM write protection (MID region 0 over bank 0) leaves the decoder's two agents (VDEC MC
 * and VLD, bits 8 and 9) out of its mask, so their writes are dropped silently. The driver
 * adds them while it is bound and puts LG's mask back on unbind.
 *
 * Not done: field pictures and MBAFF (interlaced streams are refused), 4:2:2, high bit depth,
 * FMO/ASO, SP/SI slices.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-h264.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-vmalloc.h>

#define DRV_NAME "mt5398-vdec"

/* the decoder's own layout (luma 64x32 tiles, then CbCr 64x16 tiles, rows padded to 32) */
#define V4L2_PIX_FMT_MT5398_TILED v4l2_fourcc('M', 'T', '6', '4')

#define MAX_W		1920
#define MAX_H		1088
#define MAX_BUFS	32	/* CAPTURE index = the picture id in the AMV words (bits 23+) */
#define MAX_REFS	16	/* frame decoding: AMV has 16 entries per list */
#define WORK_SIZE	0x8000

/* register blocks, in the DT's reg order */
enum { R_VLD, R_VSYS, R_MC, R_PP, R_AVLD, R_AMV, R_CKGEN, R_MID, R_NUM };
static const char *const reg_names[R_NUM] = {
	"vld", "vsys", "mc", "pp", "avld", "amv", "ckgen", "mid",
};

/* VLD; its TOP part is at +0x800 */
#define VLD_BARREL		0x08c
#define VLD_RPTR		0x0b0
#define VLD_FIFO_START		0x0b4	/* >> 6 */
#define VLD_FIFO_END		0x0b8	/* >> 6 */
#define VLD_BARREL_RDY		0x0e8	/* bit 0 */
#define VLD_BARREL_ST		0x0f4	/* bit 15 busy, bit 0 ready */
#define VLD_FETCH_W		0x0f8
#define VLD_FETCH_R		0x0fc
#define VLD_RESET		0x108
#define VLD_WPTR		0x110
#define VLD_SL_ADDR		0x260	/* scaling lists */
#define VLD_SL_DATA		0x264
#define TOP_WORK		0x828
#define TOP_WORK_CFG		0x82c
#define TOP_850			0x850
#define TOP_IRQ			0x854	/* bit 0: decode-done interrupt armed */
#define TOP_858			0x858
#define TOP_DIM			0x868	/* map units - 1 << 16 | width in MBs - 1 */
#define TOP_MBS			0x86c	/* macroblocks in the picture */

/* AVLD: the H.264 part of the VLD. +4n shifts n bits and returns the new window (+0 peeks) */
#define AVLD_UE			0x23c
#define AVLD_SE			0x240
#define AVLD_MODE		0x084
#define AVLD_SPS		0x088
#define AVLD_FIRST_MB		0x08c
#define AVLD_PPS0		0x090
#define AVLD_PPS1		0x094
#define AVLD_SHDR1		0x098
#define AVLD_SHDR2		0x09c
#define AVLD_MAX_FRAME_NUM	0x0a0	/* MaxFrameNum (x 2 for fields) */
#define AVLD_FRAME_NUM		0x0a4
#define AVLD_P_PICNUM		0x0a8	/* + 4i */
#define AVLD_B0_PICNUM		0x128
#define AVLD_B1_PICNUM		0x1a8
#define AVLD_228		0x228	/* command: write 1, reads 0 when done */
#define AVLD_REORDER		0x22c	/* 1 = parse ref_pic_list_modification(); reads != 0 when done, bit 1 error */
#define AVLD_PRED_WEIGHT	0x230	/* the same for pred_weight_table() */
#define AVLD_CABAC		0x234
#define AVLD_START		0x238
#define AVLD_ERR		0x270
#define AVLD_IRQ_ST		0x274
#define AVLD_DONE		0x278	/* bit 31 */
#define AVLD_280		0x280
#define AVLD_BARREL_MODE	0x28c
#define AVLD_IRQ_CLR		0x2c4
#define AVLD_2C8		0x2c8
#define AVLD_2D0		0x2d0
#define AVLD_2D4		0x2d4

/* AMV: motion vector unit */
#define AMV_P			0x000	/* + 8i: two words per reference */
#define AMV_B0			0x080
#define AMV_B1			0x100
#define AMV_COL			0x180	/* + 8i: list 1 reference i's MV buffer >> 4, then + 2 */
#define AMV_POC			0x200
#define AMV_POC_TOP		0x204
#define AMV_POC_BOT		0x208
#define AMV_MV			0x20c	/* this picture's MV buffer >> 4 */
#define AMV_210			0x210
#define AMV_CFG			0x220	/* | 0x20 = direct_8x8_inference */

/* MC: motion compensation and picture output */
#define MC_024			0x024
#define MC_090			0x090
#define MC_WIDTH		0x208
#define MC_HEIGHT		0x20c
#define MC_220			0x220
#define MC_LUMA			0x224	/* >> 9 */
#define MC_CHROMA		0x228	/* >> 8 */
#define MC_MB_STRIDE		0x22c
#define MC_STRUCT		0x238	/* 3 = frame */
#define MC_250			0x250
#define MC_MBW1			0x260
#define MC_MBH1			0x264
#define MC_P_LIST		0x3dc	/* + 4i: reference luma addresses, 32 entries */
#define MC_B0_LIST		0x45c
#define MC_B1_LIST		0x4dc
#define MC_CHROMA_OFF		0x55c
#define MC_LUMA_ADDR		0x560
#define MC_564			0x564
#define MC_568			0x568
#define MC_56C			0x56c
#define MC_57C			0x57c
#define MC_5E4			0x5e4
#define MC_638			0x638
#define MC_668			0x668
#define MC_66C			0x66c
#define MC_71C			0x71c
#define MC_768			0x768
#define MC_980			0x980

#define PP_00C			0x00c

#define VSYS_84			0x084
#define VSYS_C8			0x0c8
#define VSYS_CC			0x0cc

/* MID (DRAM write protection) region 0: +0 upper | enable, +4 lower, +8 agent mask */
#define MID_UPPER		0x0
#define MID_LOWER		0x4
#define MID_AGENTS		0x8
#define MID_ENABLE		BIT(31)
#define MID_VDEC_AGENTS		(BIT(8) | BIT(9))

enum { NAL_SLICE = 1, NAL_IDR = 5 };
enum { SL_P, SL_B, SL_I, SL_SP, SL_SI };

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "1 = log each job's bitstream size and slices");

static bool trace;
module_param(trace, bool, 0644);
MODULE_PARM_DESC(trace, "record register accesses in debugfs mt5398-vdec/trace");

struct vdec_aux {
	dma_addr_t mv;			/* motion vectors of the picture in this CAPTURE buffer */
	void *mv_cookie;		/* dma_alloc_attrs' return (NO_KERNEL_MAPPING): needed to free */
	void *tiled;			/* NV12 mode: the tiled picture (cached), or NULL */
	dma_addr_t tiled_dma;
	size_t mv_size, tiled_size;
};

struct vdec_dev {
	struct device *dev;
	struct v4l2_device v4l2_dev;
	struct v4l2_m2m_dev *m2m_dev;
	struct media_device mdev;
	struct video_device vdev;
	struct mutex vdev_lock;
	void __iomem *reg[R_NUM];
	phys_addr_t reg_phys[R_NUM];
	int irq;
	unsigned long decodes, irqs, irqs_empty, slices, errors;
	unsigned int irq_gen;
	u64 busy_ns;
	struct workqueue_struct *wq;
	struct work_struct run_work;
	void *work;
	dma_addr_t work_dma;
	u32 mid_saved;
	bool mid_changed;
	struct dentry *dbg;
	/* trace ring */
	spinlock_t tlock;
	u32 *tbuf;
	unsigned int tlen, tcap;
};

struct vdec_ctx {
	struct v4l2_fh fh;
	struct vdec_dev *v;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_format coded_fmt, decoded_fmt;
	struct vdec_aux aux[MAX_BUFS];
	u8 *bounce;			/* start code scan */
};

/* one decode job */
struct vdec_run {
	struct vb2_v4l2_buffer *src, *dst;
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	const struct v4l2_ctrl_h264_scaling_matrix *sm;
	const struct v4l2_ctrl_h264_decode_params *dec;
	dma_addr_t fifo, fifo_end, wptr;
	u32 fifo_size;
	dma_addr_t luma, chroma, mv;
	unsigned int mbw, mbh;		/* frame size in macroblocks */
	unsigned int stride_mbs;	/* the buffer's: tile rows are 64 pixels wide */
	u32 width, height;		/* = coded size */
	/* reference per DPB entry */
	struct {
		dma_addr_t luma, mv;
		u32 id;
	} ref[V4L2_H264_NUM_DPB_ENTRIES];
	struct v4l2_h264_reference p[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference b0[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference b1[V4L2_H264_REF_LIST_LEN];
	unsigned int nref;
};

/* slice header fields read from the bitstream */
struct vdec_slice {
	u8 nal_ref_idc, nal_type;
	u32 first_mb, slice_type_raw, frame_num;
	u8 type;			/* SL_* */
	bool direct_spatial;
	u32 num_ref_l0, num_ref_l1;	/* active - 1 */
	u32 cabac_init_idc;
	s32 qp_delta;
	u32 deblock_idc;
	s32 alpha, beta;
};

static inline struct vdec_ctx *fh_to_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct vdec_ctx, fh);
}

/* ---- register access (optionally traced, as "W addr value" / "R addr value") ---- */

static void tr(struct vdec_dev *v, char rw, int b, u32 off, u32 val)
{
	unsigned long flags;

	if (!trace || !v->tbuf)
		return;
	spin_lock_irqsave(&v->tlock, flags);
	if (v->tlen + 2 <= v->tcap) {
		v->tbuf[v->tlen++] = (u32)(v->reg_phys[b] + off) | (rw == 'R');
		v->tbuf[v->tlen++] = val;
	}
	spin_unlock_irqrestore(&v->tlock, flags);
}

static void wr(struct vdec_dev *v, int b, u32 off, u32 val)
{
	tr(v, 'W', b, off, val);
	writel(val, v->reg[b] + off);
}

static u32 rd(struct vdec_dev *v, int b, u32 off)
{
	return readl(v->reg[b] + off);
}

/* a read that changes state (bitstream) or whose value matters for the trace */
static u32 rdt(struct vdec_dev *v, int b, u32 off)
{
	u32 val = readl(v->reg[b] + off);

	tr(v, 'R', b, off, val);
	return val;
}

static void rmw(struct vdec_dev *v, int b, u32 off, u32 clr, u32 set)
{
	wr(v, b, off, (rd(v, b, off) & ~clr) | set);
}

/* ---- bitstream reader ---- */

/* LG's reader waits until the VLD has prefetched more than 8 bytes before each read */
static int bs_wait(struct vdec_dev *v, struct vdec_run *run)
{
	int i;

	for (i = 0; i < 100000; i++) {
		u32 f = rd(v, R_VLD, VLD_FETCH_R), w = rd(v, R_VLD, VLD_FETCH_W);

		if (w < f)
			w += run->fifo_size;
		if (w - f > 8)
			return 0;
	}
	dev_err(v->dev, "bitstream: VLD not fetching (r %08x w %08x)\n",
		rd(v, R_VLD, VLD_FETCH_R), rd(v, R_VLD, VLD_FETCH_W));
	return -EIO;
}

struct bs {
	struct vdec_dev *v;
	struct vdec_run *run;
	int err;
};

static u32 bs_bits(struct bs *b, unsigned int n)	/* n = 1..16 */
{
	u32 val;

	if (b->err || (b->err = bs_wait(b->v, b->run)))
		return 0;
	val = rdt(b->v, R_AVLD, 0) >> (32 - n);
	rdt(b->v, R_AVLD, 4 * n);
	return val;
}

static u32 bs_long(struct bs *b, unsigned int n)	/* n = 0..32 */
{
	if (n > 16)
		return bs_bits(b, n - 16) << 16 | bs_bits(b, 16);
	return n ? bs_bits(b, n) : 0;
}

static u32 bs_ue(struct bs *b)
{
	if (b->err || (b->err = bs_wait(b->v, b->run)))
		return 0;
	return rdt(b->v, R_AVLD, AVLD_UE);
}

static s32 bs_se(struct bs *b)
{
	if (b->err || (b->err = bs_wait(b->v, b->run)))
		return 0;
	return (s32)rdt(b->v, R_AVLD, AVLD_SE);
}

/* ---- hardware sequence ---- */

/* LG's VLD hardware reset, clock selects, list clear and ResetVLD1 */
static void hw_reset(struct vdec_dev *v)
{
	int i;

	wr(v, R_VLD, VLD_RESET, 0x101);
	wr(v, R_CKGEN, 0, 4);
	wr(v, R_VSYS, VSYS_C8, 0xf7);
	wr(v, R_VSYS, VSYS_CC, 0x13a20100);
	wr(v, R_VSYS, VSYS_84, 2);
	wr(v, R_VSYS, VSYS_84, 2);
	wr(v, R_VLD, VLD_RESET, 0);
	for (i = 0; i < 96; i++)
		wr(v, R_MC, MC_P_LIST + 4 * i, 0);
	wr(v, R_VLD, TOP_850, 0x01148000);
	wr(v, R_VLD, TOP_858, 0xffffffff);
	wr(v, R_MC, MC_638, 0);

	wr(v, R_VLD, TOP_IRQ, 1);
	wr(v, R_AVLD, AVLD_MODE, 0x6480c001);
	rmw(v, R_AVLD, AVLD_2C8, 0xe003, 1);
	rmw(v, R_AVLD, AVLD_2C8, 0, 8);
	rmw(v, R_AVLD, AVLD_2C8, 0, 0x40000);
	wr(v, R_AVLD, AVLD_2D0, 0x103);
	wr(v, R_AVLD, AVLD_2D4, 0xc210);
	wr(v, R_AMV, AMV_210, 4);
	wr(v, R_AVLD, AVLD_280, 0xbfdfb0f7);
	wr(v, R_MC, MC_024, 6);
}

/* FIFO = the OUTPUT buffer; the barrel shifter starts near the slice's start code */
static int barrel_init(struct vdec_dev *v, struct vdec_run *run, dma_addr_t rptr)
{
	u32 st;
	int ret;

	wr(v, R_VLD, VLD_FIFO_START, run->fifo >> 6);
	wr(v, R_VLD, VLD_FIFO_END, run->fifo_end >> 6);
	wr(v, R_VLD, TOP_WORK, v->work_dma);
	wr(v, R_VLD, TOP_WORK_CFG, rd(v, R_VLD, TOP_WORK_CFG));
	wr(v, R_VLD, VLD_WPTR, run->wptr);
	wr(v, R_VLD, VLD_RPTR, rptr);
	st = rd(v, R_VLD, VLD_FETCH_R);
	if (st != (rptr & ~15u)) {
		dev_err(v->dev, "VLD read pointer %08x, wanted %08x\n", st, (u32)rptr & ~15u);
		return -EIO;
	}
	wr(v, R_VLD, VLD_RESET, 0x100);
	wr(v, R_VLD, VLD_RESET, 0);
	wr(v, R_AVLD, AVLD_BARREL_MODE, 1);
	wr(v, R_VLD, VLD_BARREL, 0x100000);
	ret = readl_poll_timeout_atomic(v->reg[R_VLD] + VLD_BARREL_RDY, st, st & 1, 0, 2000);
	if (!ret)	/* bit 15 = still busy, bit 0 = ready */
		ret = readl_poll_timeout_atomic(v->reg[R_VLD] + VLD_BARREL_ST, st,
						!(st & 0x8000) || (st & 1), 0, 2000);
	if (ret) {
		dev_err(v->dev, "barrel shifter: no ready (0e8 %08x 0f4 %08x)\n",
			rd(v, R_VLD, VLD_BARREL_RDY), rd(v, R_VLD, VLD_BARREL_ST));
		return -EIO;
	}
	wr(v, R_VLD, VLD_BARREL, 0x800000);
	wr(v, R_AVLD, AVLD_BARREL_MODE, 0);
	return 0;
}

/*
 * The shifter's window starts a few bytes before the start code (it depends on the address's
 * alignment). As LG's code: read 8 bits until the window is 00 00 01 <NAL header>, then
 * consume those 4 bytes.
 */
static int bs_sync(struct bs *b, u8 nalhdr)
{
	u32 want = 0x100 | nalhdr;
	int i;

	for (i = 0; i < 64; i++) {
		if ((b->err = bs_wait(b->v, b->run)))
			return b->err;
		if (rdt(b->v, R_AVLD, 0) == want)
			break;
		rdt(b->v, R_AVLD, 4 * 8);
	}
	if (i == 64) {
		dev_err(b->v->dev, "bitstream: start code not found\n");
		return -EIO;
	}
	for (i = 0; i < 4; i++)
		bs_bits(b, 8);
	return b->err;
}

static int hw_cmd(struct vdec_dev *v, u32 reg, const char *what)
{
	u32 st = 0;
	int i;

	wr(v, R_AVLD, reg, 1);
	for (i = 0; i <= 100; i++) {
		st = rdt(v, R_AVLD, reg);
		if (st)
			break;
	}
	if (!st || (st & 2)) {
		dev_err(v->dev, "%s: hardware parse failed (%08x)\n", what, st);
		return -EIO;
	}
	return 0;
}

/* LG's AVLD+228: write 1, then up to 100 reads until it reads 0 */
static int hw_cmd_clear(struct vdec_dev *v, u32 reg)
{
	int i;

	wr(v, R_AVLD, reg, 1);
	for (i = 0; i <= 100; i++)
		if (!rd(v, R_AVLD, reg))
			return 0;
	dev_err(v->dev, "AVLD+%03x stays %08x\n", reg, rd(v, R_AVLD, reg));
	return -EIO;
}

static u32 amv_word(struct vdec_run *run, const struct v4l2_h264_reference *r, bool bottom,
		    bool with_poc)
{
	const struct v4l2_h264_dpb_entry *e = &run->dec->dpb[r->index];
	s32 top = e->top_field_order_cnt, bot = e->bottom_field_order_cnt;
	bool lt = e->flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;
	u32 w = run->ref[r->index].id << 23 | (u32)bottom << 22 | (u32)lt << 20;

	if (top <= bot && top != S32_MAX)
		w |= BIT(21);
	if (with_poc)
		w += (u32)(bottom ? bot : top) & 0x3ffff;
	return w;
}

static u32 picnum_word(struct vdec_run *run, const struct v4l2_h264_reference *r)
{
	const struct v4l2_h264_dpb_entry *e = &run->dec->dpb[r->index];
	bool lt = e->flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;

	return (u32)lt << 19 | (e->pic_num & 0x7ffff);
}

static void write_lists(struct vdec_dev *v, struct vdec_run *run, bool refs)
{
	const struct v4l2_ctrl_h264_decode_params *dec = run->dec;
	s32 top = dec->top_field_order_cnt, bot = dec->bottom_field_order_cnt;
	unsigned int i, n = refs ? run->nref : 0;

	for (i = 0; i < n; i++) {
		const struct v4l2_h264_reference *r = &run->p[i];

		wr(v, R_MC, MC_P_LIST + 4 * i, run->ref[r->index].luma);
		wr(v, R_AVLD, AVLD_P_PICNUM + 4 * i, picnum_word(run, r));
		wr(v, R_AMV, AMV_P + 8 * i, amv_word(run, r, false, false));
		wr(v, R_AMV, AMV_P + 8 * i + 4, amv_word(run, r, true, false));
	}
	wr(v, R_AMV, AMV_POC_TOP, top);
	wr(v, R_AMV, AMV_POC_BOT, bot);
	wr(v, R_AMV, AMV_POC, min(top, bot));
	for (i = 0; i < n; i++) {
		const struct v4l2_h264_reference *r0 = &run->b0[i], *r1 = &run->b1[i];
		u32 mv = run->ref[r1->index].mv >> 4;

		wr(v, R_MC, MC_B0_LIST + 4 * i, run->ref[r0->index].luma);
		wr(v, R_AVLD, AVLD_B0_PICNUM + 4 * i, picnum_word(run, r0));
		wr(v, R_AMV, AMV_B0 + 8 * i, amv_word(run, r0, false, true));
		wr(v, R_AMV, AMV_B0 + 8 * i + 4, amv_word(run, r0, true, true));
		wr(v, R_MC, MC_B1_LIST + 4 * i, run->ref[r1->index].luma);
		wr(v, R_AVLD, AVLD_B1_PICNUM + 4 * i, picnum_word(run, r1));
		wr(v, R_AMV, AMV_B1 + 8 * i, amv_word(run, r1, false, true));
		wr(v, R_AMV, AMV_B1 + 8 * i + 4, amv_word(run, r1, true, true));
		wr(v, R_AMV, AMV_COL + 8 * i, mv);
		wr(v, R_AMV, AMV_COL + 8 * i + 4, mv + 2);
	}
}

/*
 * Lists in raster order, as V4L2 gives them (LG's parser stores list[zigzag[j]]): the six 4x4
 * lists at 16 i, then Intra Y 8x8 at 0x60 and Inter Y 8x8 at 0xa0. Each word is written as
 * address + 0x200, data, address + 0x100.
 */
static void write_scaling(struct vdec_dev *v, const struct v4l2_ctrl_h264_scaling_matrix *sm)
{
	int i, k, n;

	for (i = 0; i < 8; i++) {
		const u8 *l = i < 6 ? sm->scaling_list_4x4[i] : sm->scaling_list_8x8[i - 6];
		u32 base = i < 6 ? 16 * i : i == 6 ? 0x60 : 0xa0;

		n = i < 6 ? 16 : 64;
		for (k = 0; k < n / 4; k++) {
			wr(v, R_VLD, VLD_SL_ADDR, base + 4 * k + 0x200);
			wr(v, R_VLD, VLD_SL_DATA, l[4 * k] << 24 | l[4 * k + 1] << 16 | l[4 * k + 2] << 8 | l[4 * k + 3]);
			wr(v, R_VLD, VLD_SL_ADDR, base + 4 * k + 0x100);
		}
	}
}

#define FLAG(x, f) ((u32)!!((x)->flags & (f)))

static u32 sps_word(const struct v4l2_ctrl_h264_sps *s)
{
	return (s->chroma_format_idc & 3) | 0x200000 |
	       (s->log2_max_frame_num_minus4 & 15) << 2 |
	       (s->pic_order_cnt_type & 3) << 6 |
	       (s->log2_max_pic_order_cnt_lsb_minus4 & 15) << 8 |
	       FLAG(s, V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO) << 12 |
	       (s->max_num_ref_frames & 31) << 13 |
	       FLAG(s, V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) << 18 |
	       FLAG(s, V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD) << 19 |
	       FLAG(s, V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE) << 20;
}

static u32 pps_word0(const struct v4l2_ctrl_h264_pps *p)
{
	return FLAG(p, V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) |
	       FLAG(p, V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) << 1 |
	       FLAG(p, V4L2_H264_PPS_FLAG_WEIGHTED_PRED) << 2 |
	       (p->weighted_bipred_idc & 3) << 3 |
	       (p->pic_init_qp_minus26 & 63) << 5 |
	       (p->chroma_qp_index_offset & 31) << 11 |
	       FLAG(p, V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) << 16 |
	       FLAG(p, V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED) << 17 |
	       FLAG(p, V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) << 18 |
	       (p->second_chroma_qp_index_offset & 31) << 19;
}

/*
 * The picture is done when its interrupt comes (AVLD+274 set) or AVLD+278 bit 31 is set (LG's
 * _VDEC_WaitDecDone polls it when the interrupt is late; it is missing for about 3% of
 * pictures). Then clear as LG's ISR does.
 */
static int wait_done(struct vdec_dev *v, unsigned int gen)
{
	unsigned int i;
	u32 st = 0;

	for (i = 0; i < 5000; i++) {
		st = rd(v, R_AVLD, AVLD_DONE);
		if ((st & BIT(31)) || READ_ONCE(v->irq_gen) != gen)
			break;
		usleep_range(80, 120);
	}
	tr(v, 'R', R_AVLD, AVLD_DONE, st);
	rdt(v, R_AVLD, AVLD_ERR);
	rdt(v, R_AVLD, 0x284);
	if (i == 5000) {
		dev_err(v->dev, "decode timeout: AVLD+270 %08x +274 %08x +278 %08x +284 %08x\n",
			rd(v, R_AVLD, AVLD_ERR), rd(v, R_AVLD, AVLD_IRQ_ST), st, rd(v, R_AVLD, 0x284));
		return -ETIMEDOUT;
	}
	v->decodes++;
	wr(v, R_VLD, TOP_IRQ, rd(v, R_VLD, TOP_IRQ) & ~1u);
	wr(v, R_AVLD, AVLD_IRQ_CLR, 1);
	wr(v, R_AVLD, AVLD_IRQ_CLR, 0);
	return 0;
}

static int decode_picture(struct vdec_ctx *ctx, struct vdec_run *run, u32 sc_off, u8 nalhdr)
{
	struct vdec_dev *v = ctx->v;
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	struct bs b = { .v = v, .run = run };
	struct vdec_slice s = { .nal_ref_idc = nalhdr >> 5 & 3, .nal_type = nalhdr & 31 };
	bool idr = s.nal_type == NAL_IDR, cabac = pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	bool weighted;
	unsigned int gen;
	u32 w, pps1;
	int ret;

	hw_reset(v);
	ret = barrel_init(v, run, run->fifo + sc_off);
	if (ret)
		return ret;
	ret = bs_sync(&b, nalhdr);
	if (ret)
		return ret;

	/* slice_header() up to num_ref_idx_active_override */
	s.first_mb = bs_ue(&b);
	s.slice_type_raw = bs_ue(&b);
	s.type = s.slice_type_raw % 5;
	if (bs_ue(&b) != pps->pic_parameter_set_id && !b.err) {
		dev_err(v->dev, "slice refers to another PPS\n");
		return -EINVAL;
	}
	if (sps->flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE)
		bs_bits(&b, 2);
	s.frame_num = bs_long(&b, sps->log2_max_frame_num_minus4 + 4);
	/* frame_mbs_only is required (try_ctrl), so no field_pic_flag */
	if (idr)
		bs_ue(&b);		/* idr_pic_id */
	if (sps->pic_order_cnt_type == 0) {
		bs_long(&b, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pps->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT)
			bs_se(&b);
	} else if (sps->pic_order_cnt_type == 1 &&
		   !(sps->flags & V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO)) {
		bs_se(&b);
		if (pps->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT)
			bs_se(&b);
	}
	if (pps->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT)
		bs_ue(&b);
	if (s.type == SL_B)
		s.direct_spatial = bs_bits(&b, 1);
	s.num_ref_l0 = pps->num_ref_idx_l0_default_active_minus1;
	s.num_ref_l1 = pps->num_ref_idx_l1_default_active_minus1;
	if (s.type == SL_P || s.type == SL_SP || s.type == SL_B) {
		if (bs_bits(&b, 1)) {
			s.num_ref_l0 = bs_ue(&b);
			if (s.type == SL_B)
				s.num_ref_l1 = bs_ue(&b);
		}
	}
	if (b.err)
		return b.err;
	if (s.type == SL_SP || s.type == SL_SI) {
		dev_err(v->dev, "SP/SI slices are not supported\n");
		return -EINVAL;
	}
	if (s.num_ref_l0 > 31 || s.num_ref_l1 > 31)
		return -EINVAL;

	/* as LG's order: lists, POCs, SPS/PPS/slice words */
	wr(v, R_MC, MC_564, 0);
	ret = hw_cmd_clear(v, AVLD_228);
	if (ret)
		return ret;
	wr(v, R_AVLD, AVLD_MAX_FRAME_NUM, 1u << (sps->log2_max_frame_num_minus4 + 4));
	wr(v, R_AVLD, AVLD_FRAME_NUM, s.frame_num);
	write_lists(v, run, s.type != SL_I);
	wr(v, R_MC, MC_568, 0);
	wr(v, R_MC, MC_56C, 0);
	pps1 = pps->num_ref_idx_l0_default_active_minus1 |
	       pps->num_ref_idx_l1_default_active_minus1 << 5;
	if (pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) {
		write_scaling(v, run->sm);
		pps1 |= 0x1ff << 10;
	}
	w = (sps->pic_height_in_map_units_minus1 << 16) | sps->pic_width_in_mbs_minus1;
	wr(v, R_AVLD, AVLD_SPS, sps_word(sps));
	wr(v, R_VLD, TOP_DIM, w);
	wr(v, R_AVLD, AVLD_PPS0, pps_word0(pps));
	wr(v, R_AVLD, AVLD_PPS1, pps1);
	wr(v, R_AVLD, AVLD_SHDR1, (s.slice_type_raw & 15) << 13 | (u32)s.direct_spatial << 19 |
	   (s.num_ref_l0 & 31) << 20 | (s.num_ref_l1 & 31) << 25);
	wr(v, R_AVLD, AVLD_FIRST_MB, s.first_mb & 0x1ffff);
	wr(v, R_VLD, TOP_DIM, w);

	/* the hardware parses these two itself */
	if (s.type != SL_I) {
		ret = hw_cmd(v, AVLD_REORDER, "ref_pic_list_modification");
		if (ret)
			return ret;
	}
	weighted = (s.type == SL_P && (pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED)) ||
		   (s.type == SL_B && pps->weighted_bipred_idc == 1);
	if (weighted) {
		ret = hw_cmd(v, AVLD_PRED_WEIGHT, "pred_weight_table");
		if (ret)
			return ret;
	}

	/* dec_ref_pic_marking(): only skipped */
	if (s.nal_ref_idc) {
		if (idr) {
			bs_bits(&b, 1);		/* no_output_of_prior_pics_flag */
			bs_bits(&b, 1);		/* long_term_reference_flag */
		} else if (bs_bits(&b, 1)) {	/* adaptive_ref_pic_marking_mode_flag */
			u32 op;
			int n = 0;

			while ((op = bs_ue(&b)) && !b.err && n++ < 66) {
				if (op == 1 || op == 3)
					bs_ue(&b);
				if (op == 2)
					bs_ue(&b);
				if (op == 3 || op == 6)
					bs_ue(&b);
				if (op == 4)
					bs_ue(&b);
			}
		}
	}
	if (cabac && s.type != SL_I)
		s.cabac_init_idc = bs_ue(&b);
	s.qp_delta = bs_se(&b);
	if (pps->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) {
		s.deblock_idc = bs_ue(&b);
		if (s.deblock_idc != 1) {
			s.alpha = bs_se(&b);
			s.beta = bs_se(&b);
		}
	}
	if (b.err)
		return b.err;

	/* the picture */
	wr(v, R_MC, MC_WIDTH, run->width);
	wr(v, R_MC, MC_HEIGHT, run->height);
	wr(v, R_MC, MC_220, 1);
	wr(v, R_MC, MC_LUMA, run->luma >> 9);
	wr(v, R_MC, MC_CHROMA, run->chroma >> 8);
	wr(v, R_MC, MC_250, 1);
	wr(v, R_MC, MC_STRUCT, 3);
	wr(v, R_MC, MC_MBW1, run->mbw - 1);
	wr(v, R_MC, MC_MBH1, run->mbh - 1);
	wr(v, R_MC, MC_MB_STRIDE, run->stride_mbs);
	wr(v, R_MC, MC_LUMA_ADDR, run->luma);
	wr(v, R_MC, MC_CHROMA_OFF, run->chroma - run->luma);
	wr(v, R_MC, MC_090, 2);
	wr(v, R_MC, MC_768, 2);
	wr(v, R_PP, PP_00C, 1);
	wr(v, R_MC, MC_57C, 0);
	rmw(v, R_MC, MC_5E4, 0, 0x1000);
	rmw(v, R_MC, MC_668, 0x00ffffff, 1);
	rmw(v, R_MC, MC_66C, 0x00ffffff, 1);
	wr(v, R_MC, MC_980, run->stride_mbs);
	wr(v, R_AMV, AMV_MV, run->mv >> 4);
	wr(v, R_AVLD, AVLD_SHDR2, ((u32)s.qp_delta & 127) | (s.deblock_idc & 3) << 7 |
	   ((u32)s.alpha & 15) << 9 | ((u32)s.beta & 15) << 13 | (u32)s.nal_ref_idc << 17 |
	   (u32)idr << 19 | (s.cabac_init_idc & 3) << 20);
	if (cabac)
		wr(v, R_AVLD, AVLD_CABAC, 1);
	rmw(v, R_MC, MC_71C, 0, 0x88000000);
	wr(v, R_VLD, TOP_MBS, run->mbw * run->mbh);
	rmw(v, R_MC, MC_5E4, 0, 0);
	if (sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE)
		rmw(v, R_AMV, AMV_CFG, 0, 0x20);

	gen = READ_ONCE(v->irq_gen);
	wr(v, R_AVLD, AVLD_START, 1);
	return wait_done(v, gen);
}

static irqreturn_t vdec_irq(int irq, void *priv)
{
	struct vdec_dev *v = priv;

	/* acknowledged here, the job polls irq_gen; counted in debugfs "stats" */
	if (!rd(v, R_AVLD, AVLD_IRQ_ST)) {
		v->irqs_empty++;
		return IRQ_NONE;
	}
	v->irqs++;
	tr(v, 'R', R_AVLD, AVLD_DONE, rd(v, R_AVLD, AVLD_DONE));
	WRITE_ONCE(v->irq_gen, v->irq_gen + 1);
	wr(v, R_VLD, TOP_IRQ, rd(v, R_VLD, TOP_IRQ) & ~1u);
	wr(v, R_AVLD, AVLD_IRQ_CLR, 1);
	wr(v, R_AVLD, AVLD_IRQ_CLR, 0);
	return IRQ_HANDLED;
}

/* ---- buffers ---- */

static u32 tiled_stride(u32 w) { return ALIGN(w, 64); }
static u32 tiled_luma_size(u32 w, u32 h) { return tiled_stride(w) * ALIGN(h, 32); }
static u32 mv_size(u32 w, u32 h) { return DIV_ROUND_UP(w, 16) * ALIGN(DIV_ROUND_UP(h, 16), 2) * 64; }

static bool native_fmt(struct vdec_ctx *ctx)
{
	return ctx->decoded_fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_MT5398_TILED;
}

/* untile one frame into NV12 (only the coded area) */
static void untile(u8 *dst, u32 bpl, const u8 *src, u32 w, u32 h)
{
	u32 stride = tiled_stride(w), lsize = tiled_luma_size(w, h), tx, ty, y;
	const u8 *s = src;
	u8 *dc = dst + bpl * h;

	for (ty = 0; ty < h; ty += 32)
		for (tx = 0; tx < stride; tx += 64, s += 64 * 32)
			for (y = 0; y < 32 && ty + y < h; y++)
				memcpy(dst + (ty + y) * bpl + tx, s + 64 * y, min(64u, w - min(w, tx)));
	s = src + lsize;
	for (ty = 0; ty < h / 2; ty += 16)
		for (tx = 0; tx < stride; tx += 64, s += 64 * 16)
			for (y = 0; y < 16 && ty + y < h / 2; y++)
				memcpy(dc + (ty + y) * bpl + tx, s + 64 * y, min(64u, w - min(w, tx)));
}

/* ---- the job ---- */

/* the first max slice NALs: copy through a cached bounce buffer (the OUTPUT buffer is uncached) */
#define BOUNCE 4096
static int find_slices(struct vdec_ctx *ctx, const u8 *buf, u32 len, u32 *off, u8 *hdr, int max)
{
	u8 *bb = ctx->bounce;
	u32 pos = 0, n, i, base;
	int count = 0;
	u32 zeros = 0;

	/* bb[0..2] carry the last 3 bytes of the previous chunk */
	while (pos < len) {
		n = min_t(u32, BOUNCE, len - pos);
		memcpy(bb + 3, buf + pos, n);
		base = pos;
		for (i = 3; i < n + 3; i++) {
			u8 c = bb[i];

			if (zeros >= 2 && c == 1) {
				u32 sc = base + i - 3 - 2;	/* the 00 00 01 */

				if (sc + 3 < len) {
					u8 h = buf[sc + 3];	/* one more uncached byte read */
					u8 t = h & 31;

					if (t == NAL_SLICE || t == NAL_IDR) {
						off[count] = sc;
						hdr[count++] = h;
						if (count == max)
							return count;
					}
				}
			}
			zeros = c ? 0 : zeros + 1;
		}
		pos += n;
	}
	return count;
}

static int prepare_run(struct vdec_ctx *ctx, struct vdec_run *run)
{
	struct vdec_dev *v = ctx->v;
	struct vb2_queue *cap_q = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	struct v4l2_h264_reflist_builder rb;
	struct vdec_aux *aux = &ctx->aux[run->dst->vb2_buf.index];
	unsigned int i;
	u32 len;

	run->mbw = sps->pic_width_in_mbs_minus1 + 1;
	run->mbh = sps->pic_height_in_map_units_minus1 + 1;
	run->width = run->mbw * 16;
	run->height = run->mbh * 16;
	if (run->width > ctx->decoded_fmt.fmt.pix_mp.width ||
	    run->height > ctx->decoded_fmt.fmt.pix_mp.height) {
		dev_err(v->dev, "stream %ux%u, CAPTURE format %ux%u\n", run->width, run->height,
			ctx->decoded_fmt.fmt.pix_mp.width, ctx->decoded_fmt.fmt.pix_mp.height);
		return -EINVAL;
	}
	if (!aux->mv)
		return -ENOMEM;

	run->fifo = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	run->fifo_size = ALIGN_DOWN(vb2_plane_size(&run->src->vb2_buf, 0), 64);
	run->fifo_end = run->fifo + run->fifo_size;
	len = vb2_get_plane_payload(&run->src->vb2_buf, 0);
	run->wptr = min_t(dma_addr_t, ALIGN_DOWN(run->fifo + len + 16, 16) + 0x200, run->fifo_end - 64);

	if (native_fmt(ctx))
		run->luma = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 0);
	else
		run->luma = aux->tiled_dma;
	run->chroma = run->luma + tiled_luma_size(ctx->decoded_fmt.fmt.pix_mp.width,
						  ctx->decoded_fmt.fmt.pix_mp.height);
	run->mv = aux->mv;
	run->stride_mbs = tiled_stride(ctx->decoded_fmt.fmt.pix_mp.width) / 16;

	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const struct v4l2_h264_dpb_entry *e = &run->dec->dpb[i];
		struct vb2_buffer *buf = NULL;
		u32 idx;

		if (e->flags & V4L2_H264_DPB_ENTRY_FLAG_VALID)
			buf = vb2_find_buffer(cap_q, e->reference_ts);
		idx = buf ? buf->index : run->dst->vb2_buf.index;	/* missing: conceal with itself */
		if (buf && native_fmt(ctx))
			run->ref[i].luma = vb2_dma_contig_plane_dma_addr(buf, 0);
		else
			run->ref[i].luma = ctx->aux[idx].tiled_dma;
		if (!buf && native_fmt(ctx))
			run->ref[i].luma = run->luma;
		run->ref[i].mv = ctx->aux[idx].mv;
		run->ref[i].id = idx;
	}

	v4l2_h264_init_reflist_builder(&rb, run->dec, sps, run->dec->dpb);
	v4l2_h264_build_p_ref_list(&rb, run->p);
	v4l2_h264_build_b_ref_lists(&rb, run->b0, run->b1);
	run->nref = min_t(unsigned int, rb.num_valid, MAX_REFS);
	return 0;
}

static int decode(struct vdec_ctx *ctx, struct vdec_run *run)
{
	struct vdec_dev *v = ctx->v;
	struct vdec_aux *aux = &ctx->aux[run->dst->vb2_buf.index];
	const u8 *src = vb2_plane_vaddr(&run->src->vb2_buf, 0);
	u32 len = vb2_get_plane_payload(&run->src->vb2_buf, 0), off[1];
	u8 hdr[1];
	int n, ret, err;

	if (!src)
		return -EFAULT;
	ret = prepare_run(ctx, run);
	if (ret)
		return ret;
	n = find_slices(ctx, src, len, off, hdr, ARRAY_SIZE(off));
	if (debug)
		dev_info(v->dev, "job: %u bytes, first slice at %u, NAL bytes %*ph, frame_num %u%s\n",
			 len, n ? off[0] : 0, 8, src, run->dec->frame_num,
			 run->dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC ? " IDR" : "");
	if (!n) {
		dev_err(v->dev, "no slice in the buffer (%u bytes)\n", len);
		return -EINVAL;
	}
	if (aux->tiled)
		dma_sync_single_for_device(v->dev, aux->tiled_dma, aux->tiled_size, DMA_BIDIRECTIONAL);
	/*
	 * One hardware run per picture, started at its first slice: the decoder finds and parses
	 * the following slices' headers itself (LG's driver does the same: one start, one
	 * interrupt, AVLD+278 bit 31 for a 4-slice picture; trace 2026-11-26). Starting it again at
	 * a later slice re-decodes over a finished picture and hangs it.
	 */
	err = decode_picture(ctx, run, off[0], hdr[0]);
	if (!err)
		v->slices++;
	if (aux->tiled) {
		struct v4l2_pix_format_mplane *f = &ctx->decoded_fmt.fmt.pix_mp;
		u8 *dst = vb2_plane_vaddr(&run->dst->vb2_buf, 0);

		dma_sync_single_for_cpu(v->dev, aux->tiled_dma, aux->tiled_size, DMA_BIDIRECTIONAL);
		if (!dst)
			return -EFAULT;
		untile(dst, f->plane_fmt[0].bytesperline, aux->tiled, f->width, f->height);
	}
	return err;
}

static struct v4l2_ctrl *find_ctrl(struct vdec_ctx *ctx, u32 id)
{
	return v4l2_ctrl_find(&ctx->hdl, id);
}

static void run_work(struct work_struct *work)
{
	struct vdec_dev *v = container_of(work, struct vdec_dev, run_work);
	struct vdec_ctx *ctx = v4l2_m2m_get_curr_priv(v->m2m_dev);
	struct vdec_run run = {};
	struct media_request *req;
	enum vb2_buffer_state state = VB2_BUF_STATE_DONE;
	u64 t0;

	if (WARN_ON(!ctx))
		return;
	run.src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run.dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	req = run.src->vb2_buf.req_obj.req;
	if (req)
		v4l2_ctrl_request_setup(req, &ctx->hdl);
	v4l2_m2m_buf_copy_metadata(run.src, run.dst, true);

	run.sps = find_ctrl(ctx, V4L2_CID_STATELESS_H264_SPS)->p_cur.p;
	run.pps = find_ctrl(ctx, V4L2_CID_STATELESS_H264_PPS)->p_cur.p;
	run.sm = find_ctrl(ctx, V4L2_CID_STATELESS_H264_SCALING_MATRIX)->p_cur.p;
	run.dec = find_ctrl(ctx, V4L2_CID_STATELESS_H264_DECODE_PARAMS)->p_cur.p;

	t0 = ktime_get_ns();
	if (decode(ctx, &run)) {
		state = VB2_BUF_STATE_ERROR;
		v->errors++;
	}
	v->busy_ns += ktime_get_ns() - t0;
	if (state == VB2_BUF_STATE_DONE)
		vb2_set_plane_payload(&run.dst->vb2_buf, 0, ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
	if (req)
		v4l2_ctrl_request_complete(req, &ctx->hdl);
	v4l2_m2m_buf_done_and_job_finish(v->m2m_dev, ctx->fh.m2m_ctx, state);
}

static void vdec_device_run(void *priv)
{
	struct vdec_ctx *ctx = priv;

	queue_work(ctx->v->wq, &ctx->v->run_work);
}

static const struct v4l2_m2m_ops vdec_m2m_ops = {
	.device_run = vdec_device_run,
};

/* ---- controls ---- */

static int vdec_try_ctrl(struct v4l2_ctrl *ctrl)
{
	if (ctrl->id == V4L2_CID_STATELESS_H264_SPS) {
		const struct v4l2_ctrl_h264_sps *s = ctrl->p_new.p_h264_sps;

		if (s->chroma_format_idc != 1 || s->bit_depth_luma_minus8 || s->bit_depth_chroma_minus8)
			return -EINVAL;
		/* interlaced (field pictures, MBAFF) not done yet */
		if (!(s->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY))
			return -EINVAL;
		if ((s->pic_width_in_mbs_minus1 + 1) * 16 > MAX_W ||
		    (s->pic_height_in_map_units_minus1 + 1) * 16 > MAX_H)
			return -EINVAL;
	} else if (ctrl->id == V4L2_CID_STATELESS_H264_PPS) {
		if (ctrl->p_new.p_h264_pps->num_slice_groups_minus1)
			return -EINVAL;
	}
	return 0;
}

static const struct v4l2_ctrl_ops vdec_ctrl_ops = {
	.try_ctrl = vdec_try_ctrl,
};

static const struct v4l2_ctrl_config vdec_ctrls[] = {
	{ .id = V4L2_CID_STATELESS_H264_SPS, .ops = &vdec_ctrl_ops },
	{ .id = V4L2_CID_STATELESS_H264_PPS, .ops = &vdec_ctrl_ops },
	{ .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX },
	{ .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS },
	{
		.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		.min = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.max = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.def = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
	},
	{
		.id = V4L2_CID_STATELESS_H264_START_CODE,
		.min = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.max = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.def = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
		.min = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE,
		.max = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
		.menu_skip_mask = BIT(V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED),
		.def = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN,
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL,
		.min = V4L2_MPEG_VIDEO_H264_LEVEL_1_0,
		.max = V4L2_MPEG_VIDEO_H264_LEVEL_4_2,
		.def = V4L2_MPEG_VIDEO_H264_LEVEL_4_1,
	},
};

/* ---- formats ---- */

static void fill_decoded(struct v4l2_pix_format_mplane *f, u32 fourcc, u32 w, u32 h)
{
	w = clamp(ALIGN(w, 16), 48u, (u32)MAX_W);
	h = clamp(ALIGN(h, 16), 48u, (u32)MAX_H);
	f->pixelformat = fourcc;
	f->width = w;
	f->height = h;
	f->field = V4L2_FIELD_NONE;
	f->num_planes = 1;
	memset(f->plane_fmt, 0, sizeof(f->plane_fmt));
	if (fourcc == V4L2_PIX_FMT_MT5398_TILED) {
		f->plane_fmt[0].bytesperline = tiled_stride(w);
		f->plane_fmt[0].sizeimage = tiled_luma_size(w, h) * 3 / 2;
	} else {
		f->plane_fmt[0].bytesperline = w;
		f->plane_fmt[0].sizeimage = w * h * 3 / 2;
	}
}

static void fill_coded(struct v4l2_pix_format_mplane *f)
{
	u32 min = max_t(u32, f->width * f->height * 3 / 4, SZ_512K);

	f->pixelformat = V4L2_PIX_FMT_H264_SLICE;
	f->width = clamp(ALIGN(f->width, 16), 48u, (u32)MAX_W);
	f->height = clamp(ALIGN(f->height, 16), 48u, (u32)MAX_H);
	f->field = V4L2_FIELD_NONE;
	f->num_planes = 1;
	f->plane_fmt[0].bytesperline = 0;
	f->plane_fmt[0].sizeimage = PAGE_ALIGN(clamp_t(u32, f->plane_fmt[0].sizeimage, min, SZ_8M));
}

static int vdec_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "MT5398 H.264 decoder", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" DRV_NAME, sizeof(cap->bus_info));
	return 0;
}

static int vdec_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fs)
{
	if (fs->index || fs->pixel_format != V4L2_PIX_FMT_H264_SLICE)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise.min_width = 48;
	fs->stepwise.max_width = MAX_W;
	fs->stepwise.step_width = 16;
	fs->stepwise.min_height = 48;
	fs->stepwise.max_height = MAX_H;
	fs->stepwise.step_height = 16;
	return 0;
}

static int vdec_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_H264_SLICE;
	return 0;
}

static int vdec_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index == 0) {
		f->pixelformat = V4L2_PIX_FMT_NV12;
		return 0;
	}
	if (f->index == 1) {
		f->pixelformat = V4L2_PIX_FMT_MT5398_TILED;
		strscpy(f->description, "MT5398 Y/UV 4:2:0 64x32 tiles", sizeof(f->description));
		return 0;
	}
	return -EINVAL;
}

static int vdec_g_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	*f = fh_to_ctx(priv)->coded_fmt;
	return 0;
}

static int vdec_g_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	*f = fh_to_ctx(priv)->decoded_fmt;
	return 0;
}

static int vdec_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	fill_coded(&f->fmt.pix_mp);
	return 0;
}

static int vdec_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vdec_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;
	u32 fourcc = p->pixelformat == V4L2_PIX_FMT_MT5398_TILED ? p->pixelformat : V4L2_PIX_FMT_NV12;

	/* the size follows the coded format */
	fill_decoded(p, fourcc, ctx->coded_fmt.fmt.pix_mp.width, ctx->coded_fmt.fmt.pix_mp.height);
	return 0;
}

static int vdec_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vdec_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct v4l2_pix_format_mplane *cap = &ctx->decoded_fmt.fmt.pix_mp;

	if (vb2_is_busy(v4l2_m2m_get_vq(m2m, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)) ||
	    vb2_is_busy(v4l2_m2m_get_vq(m2m, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)))
		return -EBUSY;
	fill_coded(&f->fmt.pix_mp);
	ctx->coded_fmt = *f;
	fill_decoded(cap, cap->pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height);
	cap->colorspace = f->fmt.pix_mp.colorspace;
	cap->xfer_func = f->fmt.pix_mp.xfer_func;
	cap->ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
	cap->quantization = f->fmt.pix_mp.quantization;
	return 0;
}

/*
 * NV12: the decoder writes its own tiled buffer and the CPU untiles into the CAPTURE buffer,
 * so that can be ordinary cached memory (vmalloc). With dma-contig it would be mapped
 * uncached and every CPU reader downstream (videoconvert: ~45 ms per 720p frame) crawls.
 * The tiled format is the decoder's own output and stays dma-contig.
 */
static void set_cap_memops(struct vdec_ctx *ctx)
{
	struct vb2_queue *q = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

	q->mem_ops = native_fmt(ctx) ? &vb2_dma_contig_memops : &vb2_vmalloc_memops;
}

static int vdec_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vdec_ctx *ctx = fh_to_ctx(priv);

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)))
		return -EBUSY;
	vdec_try_fmt_cap(file, priv, f);
	ctx->decoded_fmt = *f;
	set_cap_memops(ctx);
	return 0;
}

static const struct v4l2_ioctl_ops vdec_ioctl_ops = {
	.vidioc_querycap = vdec_querycap,
	.vidioc_enum_framesizes = vdec_enum_framesizes,
	.vidioc_enum_fmt_vid_out = vdec_enum_fmt_out,
	.vidioc_enum_fmt_vid_cap = vdec_enum_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane = vdec_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = vdec_g_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = vdec_try_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = vdec_try_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = vdec_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = vdec_s_fmt_cap,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_decoder_cmd = v4l2_m2m_ioctl_stateless_decoder_cmd,
	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_stateless_try_decoder_cmd,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ---- queues ---- */

static int vdec_queue_setup(struct vb2_queue *q, unsigned int *nbufs, unsigned int *nplanes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(q);
	struct v4l2_pix_format_mplane *f = V4L2_TYPE_IS_OUTPUT(q->type) ?
		&ctx->coded_fmt.fmt.pix_mp : &ctx->decoded_fmt.fmt.pix_mp;

	if (*nplanes)
		return *nplanes != 1 || sizes[0] < f->plane_fmt[0].sizeimage ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = f->plane_fmt[0].sizeimage;
	return 0;
}

static int vdec_buf_init(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane *f = &ctx->decoded_fmt.fmt.pix_mp;
	struct vdec_aux *aux;
	struct device *dev = ctx->v->dev;

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type))
		return 0;
	if (vb->index >= MAX_BUFS)
		return -EINVAL;
	aux = &ctx->aux[vb->index];
	aux->mv_size = mv_size(f->width, f->height);
	aux->mv_cookie = dma_alloc_attrs(dev, aux->mv_size, &aux->mv, GFP_KERNEL, DMA_ATTR_NO_KERNEL_MAPPING);
	if (!aux->mv_cookie)
		goto nomem;
	if (!native_fmt(ctx)) {
		aux->tiled_size = tiled_luma_size(f->width, f->height) * 3 / 2;
		aux->tiled = dma_alloc_noncoherent(dev, aux->tiled_size, &aux->tiled_dma,
						   DMA_BIDIRECTIONAL, GFP_KERNEL);
		if (!aux->tiled) {
			dma_free_attrs(dev, aux->mv_size, aux->mv_cookie, aux->mv, DMA_ATTR_NO_KERNEL_MAPPING);
			goto nomem;
		}
	}
	return 0;
nomem:
	memset(aux, 0, sizeof(*aux));
	return -ENOMEM;
}

static void vdec_buf_cleanup(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct device *dev = ctx->v->dev;
	struct vdec_aux *aux;

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type) || vb->index >= MAX_BUFS)
		return;
	aux = &ctx->aux[vb->index];
	if (aux->tiled)
		dma_free_noncoherent(dev, aux->tiled_size, aux->tiled, aux->tiled_dma, DMA_BIDIRECTIONAL);
	if (aux->mv_cookie)
		dma_free_attrs(dev, aux->mv_size, aux->mv_cookie, aux->mv, DMA_ATTR_NO_KERNEL_MAPPING);
	memset(aux, 0, sizeof(*aux));
}

static int vdec_buf_prepare(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane *f = V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type) ?
		&ctx->coded_fmt.fmt.pix_mp : &ctx->decoded_fmt.fmt.pix_mp;

	if (vb2_plane_size(vb, 0) < f->plane_fmt[0].sizeimage)
		return -EINVAL;
	if (V4L2_TYPE_IS_CAPTURE(vb->vb2_queue->type))
		vb2_set_plane_payload(vb, 0, f->plane_fmt[0].sizeimage);
	return 0;
}

static void vdec_buf_queue(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int vdec_buf_out_validate(struct vb2_buffer *vb)
{
	to_vb2_v4l2_buffer(vb)->field = V4L2_FIELD_NONE;
	return 0;
}

static void vdec_buf_request_complete(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->hdl);
}

static void vdec_stop_streaming(struct vb2_queue *q)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vb;

	for (;;) {
		vb = V4L2_TYPE_IS_OUTPUT(q->type) ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
						    v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vb)
			break;
		v4l2_ctrl_request_complete(vb->vb2_buf.req_obj.req, &ctx->hdl);
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops vdec_qops = {
	.queue_setup = vdec_queue_setup,
	.buf_init = vdec_buf_init,
	.buf_cleanup = vdec_buf_cleanup,
	.buf_prepare = vdec_buf_prepare,
	.buf_queue = vdec_buf_queue,
	.buf_out_validate = vdec_buf_out_validate,
	.buf_request_complete = vdec_buf_request_complete,
	.stop_streaming = vdec_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int vdec_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct vdec_ctx *ctx = priv;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src->io_modes = VB2_MMAP | VB2_DMABUF;
	src->drv_priv = ctx;
	src->ops = &vdec_qops;
	src->mem_ops = &vb2_dma_contig_memops;
	src->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &ctx->v->vdev_lock;
	src->dev = ctx->v->dev;
	src->supports_requests = true;
	src->requires_requests = true;
	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->drv_priv = ctx;
	dst->ops = &vdec_qops;
	dst->mem_ops = &vb2_vmalloc_memops;	/* NV12 by default: see set_cap_memops() */
	dst->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &ctx->v->vdev_lock;
	dst->dev = ctx->v->dev;
	dst->max_num_buffers = MAX_BUFS;
	return vb2_queue_init(dst);
}

/* ---- file ---- */

static int vdec_open(struct file *file)
{
	struct vdec_dev *v = video_drvdata(file);
	struct vdec_ctx *ctx;
	unsigned int i;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->bounce = kmalloc(BOUNCE + 3, GFP_KERNEL);
	if (!ctx->bounce) {
		kfree(ctx);
		return -ENOMEM;
	}
	ctx->v = v;
	v4l2_fh_init(&ctx->fh, video_devdata(file));

	v4l2_ctrl_handler_init(&ctx->hdl, ARRAY_SIZE(vdec_ctrls));
	for (i = 0; i < ARRAY_SIZE(vdec_ctrls); i++)
		v4l2_ctrl_new_custom(&ctx->hdl, &vdec_ctrls[i], NULL);
	ret = ctx->hdl.error;
	if (ret)
		goto err_hdl;
	ctx->fh.ctrl_handler = &ctx->hdl;

	ctx->coded_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ctx->coded_fmt.fmt.pix_mp.width = 1280;
	ctx->coded_fmt.fmt.pix_mp.height = 720;
	ctx->coded_fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	fill_coded(&ctx->coded_fmt.fmt.pix_mp);
	ctx->decoded_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ctx->decoded_fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	fill_decoded(&ctx->decoded_fmt.fmt.pix_mp, V4L2_PIX_FMT_NV12, 1280, 720);

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(v->m2m_dev, ctx, vdec_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_hdl;
	}
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);
	return 0;

err_hdl:
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx->bounce);
	kfree(ctx);
	return ret;
}

static int vdec_release(struct file *file)
{
	struct vdec_ctx *ctx = fh_to_ctx(file->private_data);

	v4l2_fh_del(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx->bounce);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations vdec_fops = {
	.owner = THIS_MODULE,
	.open = vdec_open,
	.release = vdec_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int vdec_request_validate(struct media_request *req)
{
	unsigned int n = vb2_request_buffer_cnt(req);

	if (!n)
		return -ENOENT;
	if (n > 1)
		return -EINVAL;
	return vb2_request_validate(req);
}

static const struct media_device_ops vdec_media_ops = {
	.req_validate = vdec_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

/* ---- debugfs trace: "W|R addr value" per access since the last write to the file ---- */

static int trace_show(struct seq_file *m, void *p)
{
	struct vdec_dev *v = m->private;
	unsigned int i;

	for (i = 0; i + 1 < v->tlen; i += 2)
		seq_printf(m, "%c %08x %08x\n", v->tbuf[i] & 1 ? 'R' : 'W', v->tbuf[i] & ~1u, v->tbuf[i + 1]);
	return 0;
}

static int stats_show(struct seq_file *m, void *p)
{
	struct vdec_dev *v = m->private;

	seq_printf(m, "errors %lu pictures %lu decodes %lu irqs %lu irqs-empty %lu busy-ms %llu\n",
		   v->errors, v->slices, v->decodes, v->irqs, v->irqs_empty, div_u64(v->busy_ns, 1000000));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static int trace_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, trace_show, inode->i_private, 64 + 20 * (size_t)((struct vdec_dev *)inode->i_private)->tlen);
}

static ssize_t trace_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	struct vdec_dev *v = ((struct seq_file *)file->private_data)->private;
	unsigned long flags;

	spin_lock_irqsave(&v->tlock, flags);
	v->tlen = 0;
	spin_unlock_irqrestore(&v->tlock, flags);
	return len;
}

static const struct file_operations trace_fops = {
	.owner = THIS_MODULE,
	.open = trace_open,
	.read = seq_read,
	.write = trace_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* ---- probe ---- */

static void mid_open(struct vdec_dev *v)
{
	u32 up = rd(v, R_MID, MID_UPPER), agents = rd(v, R_MID, MID_AGENTS);

	if (!(up & MID_ENABLE) || (agents & MID_VDEC_AGENTS) == MID_VDEC_AGENTS) {
		dev_info(v->dev, "MID region 0: %08x %08x agents %08x, nothing to change\n",
			 up, rd(v, R_MID, MID_LOWER), agents);
		return;
	}
	v->mid_saved = agents;
	v->mid_changed = true;
	wr(v, R_MID, MID_AGENTS, agents | MID_VDEC_AGENTS);
	dev_info(v->dev, "MID region 0: agents %08x -> %08x (decoder writes allowed)\n",
		 agents, rd(v, R_MID, MID_AGENTS));
}

static void mid_close(struct vdec_dev *v)
{
	if (v->mid_changed)
		wr(v, R_MID, MID_AGENTS, v->mid_saved);
	v->mid_changed = false;
}

static int vdec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vdec_dev *v;
	int i, ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->dev = dev;
	mutex_init(&v->vdev_lock);
	spin_lock_init(&v->tlock);
	INIT_WORK(&v->run_work, run_work);
	platform_set_drvdata(pdev, v);

	for (i = 0; i < R_NUM; i++) {
		struct resource *r = platform_get_resource_byname(pdev, IORESOURCE_MEM, reg_names[i]);

		if (!r)
			return dev_err_probe(dev, -EINVAL, "no %s registers\n", reg_names[i]);
		v->reg_phys[i] = r->start;
		/* not devm_ioremap_resource: CKGEN and the DRAM controller are shared blocks */
		v->reg[i] = devm_ioremap(dev, r->start, resource_size(r));
		if (!v->reg[i])
			return -ENOMEM;
	}
	v->irq = platform_get_irq(pdev, 0);
	if (v->irq < 0)
		return v->irq;

	ret = of_reserved_mem_device_init(dev);
	if (ret)
		return dev_err_probe(dev, ret, "no memory-region\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_mem;
	v->work = dma_alloc_coherent(dev, WORK_SIZE, &v->work_dma, GFP_KERNEL);
	if (!v->work) {
		ret = -ENOMEM;
		goto err_mem;
	}
	v->tcap = 2 * 65536;
	v->tbuf = vmalloc(v->tcap * sizeof(u32));

	/* quiet the decoder before taking its interrupt (stock may have left it armed) */
	wr(v, R_VLD, TOP_IRQ, rd(v, R_VLD, TOP_IRQ) & ~1u);
	wr(v, R_AVLD, AVLD_IRQ_CLR, 1);
	wr(v, R_AVLD, AVLD_IRQ_CLR, 0);
	ret = devm_request_irq(dev, v->irq, vdec_irq, 0, DRV_NAME, v);
	if (ret)
		goto err_work;

	v->wq = alloc_ordered_workqueue(DRV_NAME, 0);
	if (!v->wq) {
		ret = -ENOMEM;
		goto err_work;
	}

	ret = v4l2_device_register(dev, &v->v4l2_dev);
	if (ret)
		goto err_wq;
	v->m2m_dev = v4l2_m2m_init(&vdec_m2m_ops);
	if (IS_ERR(v->m2m_dev)) {
		ret = PTR_ERR(v->m2m_dev);
		goto err_v4l2;
	}
	v->mdev.dev = dev;
	strscpy(v->mdev.model, DRV_NAME, sizeof(v->mdev.model));
	strscpy(v->mdev.bus_info, "platform:" DRV_NAME, sizeof(v->mdev.bus_info));
	media_device_init(&v->mdev);
	v->mdev.ops = &vdec_media_ops;
	v->v4l2_dev.mdev = &v->mdev;

	v->vdev.lock = &v->vdev_lock;
	v->vdev.v4l2_dev = &v->v4l2_dev;
	v->vdev.fops = &vdec_fops;
	v->vdev.release = video_device_release_empty;
	v->vdev.vfl_dir = VFL_DIR_M2M;
	v->vdev.device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE;
	v->vdev.ioctl_ops = &vdec_ioctl_ops;
	strscpy(v->vdev.name, DRV_NAME, sizeof(v->vdev.name));
	video_set_drvdata(&v->vdev, v);

	mid_open(v);
	ret = video_register_device(&v->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_mc;
	ret = v4l2_m2m_register_media_controller(v->m2m_dev, &v->vdev, MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret)
		goto err_vdev;
	ret = media_device_register(&v->mdev);
	if (ret)
		goto err_m2m_mc;

	v->dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("trace", 0600, v->dbg, v, &trace_fops);
	debugfs_create_file("stats", 0400, v->dbg, v, &stats_fops);
	dev_info(dev, "H.264 decoder on /dev/video%d, irq %d, work buffer %pad\n",
		 v->vdev.num, v->irq, &v->work_dma);
	return 0;

err_m2m_mc:
	v4l2_m2m_unregister_media_controller(v->m2m_dev);
err_vdev:
	video_unregister_device(&v->vdev);
err_mc:
	mid_close(v);
	media_device_cleanup(&v->mdev);
	v4l2_m2m_release(v->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&v->v4l2_dev);
err_wq:
	destroy_workqueue(v->wq);
err_work:
	vfree(v->tbuf);
	dma_free_coherent(dev, WORK_SIZE, v->work, v->work_dma);
err_mem:
	of_reserved_mem_device_release(dev);
	return ret;
}

static void vdec_remove(struct platform_device *pdev)
{
	struct vdec_dev *v = platform_get_drvdata(pdev);

	debugfs_remove_recursive(v->dbg);
	media_device_unregister(&v->mdev);
	v4l2_m2m_unregister_media_controller(v->m2m_dev);
	video_unregister_device(&v->vdev);
	media_device_cleanup(&v->mdev);
	v4l2_m2m_release(v->m2m_dev);
	v4l2_device_unregister(&v->v4l2_dev);
	destroy_workqueue(v->wq);
	wr(v, R_VLD, TOP_IRQ, rd(v, R_VLD, TOP_IRQ) & ~1u);
	devm_free_irq(v->dev, v->irq, v);
	mid_close(v);
	vfree(v->tbuf);
	dma_free_coherent(v->dev, WORK_SIZE, v->work, v->work_dma);
	of_reserved_mem_device_release(v->dev);
}

static const struct of_device_id vdec_of_match[] = {
	{ .compatible = "mediatek,mt5398-vdec" },
	{ }
};
MODULE_DEVICE_TABLE(of, vdec_of_match);

static struct platform_driver vdec_driver = {
	.probe = vdec_probe,
	.remove = vdec_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = vdec_of_match,
	},
};
module_platform_driver(vdec_driver);

MODULE_DESCRIPTION("MediaTek MT5398 H.264 stateless decoder");
MODULE_LICENSE("GPL");
