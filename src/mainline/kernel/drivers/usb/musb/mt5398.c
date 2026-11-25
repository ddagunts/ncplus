// SPDX-License-Identifier: GPL-2.0
/*
 * MUSB glue for the MediaTek MT5398 TV SoC (LG NetCast 4.x), host mode, PIO.
 *
 * The MT53xx has four single-port Mentor MUSB (MHDRC) cores at 0xf0050000 + n * 0x1000,
 * one UTMI PHY block each at 0xf0059800 + n * 0x100. Register behaviour comes from the GPL
 * MediaTek driver in LG's/Vizio's 3.0 kernels (drivers/usb/host/mtk_hcd.c, mtk_cust.c):
 *
 *  - Core register WRITES must be 32 bits wide. 8/16-bit writes are done as a 32-bit
 *    read-modify-write, and the W1C interrupt status fields that share a word with the
 *    written field (INTRTX in word 0x00, INTRRX in 0x04, INTRUSB in 0x08) are written as 0
 *    so that the RMW does not acknowledge them. Reads of any width work.
 *  - INTRUSB/INTRTX/INTRRX are write-1-to-clear, not clear-on-read.
 *  - The FIFO windows (0x20 + 4 * ep) take 32-bit accesses only; reg 0x690 selects how many
 *    bytes one access moves (0 = 1, 1 = 2, 2 = 4).
 *  - Endpoint registers are flat at 0x100 + 0x10 * ep, multipoint bus control at
 *    0x480 + 8 * ep (as on the MT2701 core in mediatek.c).
 *  - Data toggles are kept in 0x80 (RX) / 0x84 (TX): bit ep = toggle, bit 16 + ep = write
 *    enable for that bit.
 *
 * The stock kernel hands over with the cores running: VBUS on (board GPIOs, left alone
 * here), devices enumerated and possibly an HSDMA channel armed. init() stops all DMA
 * channels before anything else, then repeats the stock PHY bring-up.
 *
 * Copyright (C) 2026 the netcast project contributors
 */
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>
#include "musb_core.h"

/* MT53xx-specific core registers */
#define MT_L1INTS		0x00a0	/* level-1 interrupt status */
#define MT_L1INTM		0x00a4	/* level-1 interrupt enable */
#define   MT_L1_TX		BIT(0)
#define   MT_L1_RX		BIT(1)
#define   MT_L1_USBCOM		BIT(2)
#define   MT_L1_DMA		BIT(3)
#define   MT_L1_QMU		BIT(5)
#define MT_RXTOG		0x0080
#define MT_TXTOG		0x0084
#define MT_PERF3		0x0074	/* byte */
#define MT_HUBCFG		0x0071	/* byte; bit 2: FS hub + LS device */
#define MT_TM1			0x0604	/* bit 0: fast OTG timers (TM1_EN) */
#define MT_FIFOBYTECNT		0x0690	/* bytes per FIFO access: 0 = 1, 1 = 2, 2 = 4 */
#define MT_HSDMA_INTR		0x0200	/* [7:0] status, [15:8] mask, [23:16] mask clr, [31:24] mask set */
#define MT_HSDMA_CTRL(ch)	(0x0204 + 0x10 * (ch))
#define MT_HSDMA_CHANNELS	4

/* PHY registers (per port) */
#define PHY_CTL0		0x00
#define PHY_SLEW		0x10	/* [7:0] FS/LS slew, [18:16] HS slew */
#define PHY_CTL7		0x1c
#define PHY_RESET		0x68
#define PHY_OTG			0x6c

struct mt5398_glue {
	struct device		*dev;
	struct platform_device	*musb_pdev;
	void __iomem		*phy;
};

/* ---- 32-bit-write register access ------------------------------------------------ */

/*
 * The accessors only get (base, offset) and the base may be an endpoint window, so the
 * register is identified from the address itself: every core is 4K aligned and so is its
 * ioremap, hence the low 12 bits of the address are the register offset.
 */
static u32 mt_w1c_mask(void __iomem *word)
{
	switch ((unsigned long)word & 0xfff) {
	case 0x00: return 0xffff0000;	/* INTRTX */
	case 0x04: return 0x0000ffff;	/* INTRRX */
	case 0x08: return 0x00ff0000;	/* INTRUSB */
	default:   return 0;
	}
}

static void mt_rmw(void __iomem *addr, u32 offset, u32 width_mask, u32 data)
{
	void __iomem *word = addr + (offset & ~3);
	unsigned int shift = (offset & 3) * 8;
	u32 v;

	v = readl(word) & ~mt_w1c_mask(word);
	v &= ~(width_mask << shift);
	v |= (data & width_mask) << shift;
	writel(v, word);
}

static void mt5398_writeb(void __iomem *addr, u32 offset, u8 data)
{
	mt_rmw(addr, offset, 0xff, data);
}

static void mt5398_writew(void __iomem *addr, u32 offset, u16 data)
{
	mt_rmw(addr, offset, 0xffff, data);
}

static u8 mt5398_clearb(void __iomem *addr, u32 offset)
{
	u8 v = readb(addr + offset);

	if (v)
		mt5398_writeb(addr, offset, v);	/* W1C */
	return v;
}

static u16 mt5398_clearw(void __iomem *addr, u32 offset)
{
	u16 v = readw(addr + offset);

	if (v)
		mt5398_writew(addr, offset, v);	/* W1C */
	return v;
}

/* ---- FIFOs: 32-bit accesses, 0x690 sets the width of the tail ---------------------- */

static void mt5398_read_fifo(struct musb_hw_ep *hw_ep, u16 len, u8 *dst)
{
	void __iomem *mbase = hw_ep->musb->mregs;
	void __iomem *fifo = hw_ep->fifo;
	u32 v;

	if (!len)
		return;
	if (IS_ALIGNED((unsigned long)dst, 4)) {
		ioread32_rep(fifo, dst, len >> 2);
		dst += len & ~3;
	} else {
		for (; len >= 4; len -= 4, dst += 4)
			put_unaligned(readl(fifo), (u32 *)dst);
	}
	len &= 3;
	if (len >= 2) {
		writel(1, mbase + MT_FIFOBYTECNT);
		v = readl(fifo);
		dst[0] = v;
		dst[1] = v >> 8;
		dst += 2;
		len -= 2;
	}
	if (len) {
		writel(0, mbase + MT_FIFOBYTECNT);
		dst[0] = readl(fifo);
	}
	writel(2, mbase + MT_FIFOBYTECNT);
}

static void mt5398_write_fifo(struct musb_hw_ep *hw_ep, u16 len, const u8 *src)
{
	void __iomem *mbase = hw_ep->musb->mregs;
	void __iomem *fifo = hw_ep->fifo;

	if (!len)
		return;
	if (IS_ALIGNED((unsigned long)src, 4)) {
		iowrite32_rep(fifo, src, len >> 2);
		src += len & ~3;
	} else {
		for (; len >= 4; len -= 4, src += 4)
			writel(get_unaligned((const u32 *)src), fifo);
	}
	len &= 3;
	if (len >= 2) {
		writel(1, mbase + MT_FIFOBYTECNT);
		writel(src[0] | src[1] << 8, fifo);
		src += 2;
		len -= 2;
	}
	if (len) {
		writel(0, mbase + MT_FIFOBYTECNT);
		writel(src[0], fifo);
	}
	writel(2, mbase + MT_FIFOBYTECNT);
}

/* ---- data toggles ------------------------------------------------------------------- */

static u16 mt5398_get_toggle(struct musb_qh *qh, int is_out)
{
	struct musb *musb = qh->hw_ep->musb;
	u8 ep = qh->hw_ep->epnum;

	return (readl(musb->mregs + (is_out ? MT_TXTOG : MT_RXTOG)) >> ep) & 1;
}

/*
 * As the stock driver: toggle 0 = the CSR's CLRDATATOG; toggle 1 = write-enabled set in the
 * toggle register, then a write without enables. Returns extra TXCSR bits (TX only: the
 * core never calls this for RX).
 */
static u16 mt5398_set_toggle(struct musb_qh *qh, int is_out, struct urb *urb)
{
	struct musb *musb = qh->hw_ep->musb;
	u8 ep = qh->hw_ep->epnum;
	void __iomem *reg = musb->mregs + (is_out ? MT_TXTOG : MT_RXTOG);

	if (!usb_gettoggle(urb->dev, qh->epnum, is_out))
		return is_out ? MUSB_TXCSR_CLRDATATOG : 0;
	writel(BIT(16 + ep) | BIT(ep), reg);
	writel(BIT(ep), reg);
	return 0;
}

/* ---- interrupts --------------------------------------------------------------------- */

static irqreturn_t mt5398_musb_interrupt(int irq, void *__hci)
{
	struct musb *musb = __hci;
	void __iomem *mbase = musb->mregs;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;

	spin_lock_irqsave(&musb->lock, flags);

	/* W1C; status bits latch whether enabled or not, so mask like the stock ISR */
	musb->int_usb = musb_clearb(mbase, MUSB_INTRUSB) & readb(mbase + MUSB_INTRUSBE);
	musb->int_tx = musb_clearw(mbase, MUSB_INTRTX) & readw(mbase + MUSB_INTRTXE);
	musb->int_rx = musb_clearw(mbase, MUSB_INTRRX) & readw(mbase + MUSB_INTRRXE);

	/* the stock driver treats connect + disconnect in one go as a disconnect */
	if ((musb->int_usb & (MUSB_INTR_CONNECT | MUSB_INTR_DISCONNECT)) ==
	    (MUSB_INTR_CONNECT | MUSB_INTR_DISCONNECT))
		musb->int_usb &= ~MUSB_INTR_CONNECT;

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		ret = musb_interrupt(musb);

	spin_unlock_irqrestore(&musb->lock, flags);
	return ret;
}

/* ---- bring-up ----------------------------------------------------------------------- */

static void mt5398_quiesce(struct musb *musb)
{
	void __iomem *mbase = musb->mregs;
	int ch;

	writel(0, mbase + MT_L1INTM);
	for (ch = 0; ch < MT_HSDMA_CHANNELS; ch++)
		writel(0, mbase + MT_HSDMA_CTRL(ch));
	/* ack DMA status, clear the DMA interrupt mask */
	writel(0x00ff00ff, mbase + MT_HSDMA_INTR);
}

static void phy_rmw(void __iomem *phy, u32 reg, u32 clr, u32 set)
{
	writel((readl(phy + reg) & ~clr) | set, phy + reg);
}

/*
 * MGC_PhyReset() of LG's MT5398 driver (GPL kernel-m13, drivers/usb/host/mtk_cust.c), minus
 * the global USB DMA enable. Checked against the stock kernel's steady state on the TV
 * (2026-11-23, all four ports): PHY+0x10 = 0x00045511 (slew rates below), TM1 (0x604)
 * bit 0 clear, PHY+0x18 without Vizio's 0x60000 "disconnect threshold", so that is not set.
 * LG also delays the OTG bits by 300 ms on the Wi-Fi port (port 3) so the built-in dongle
 * re-enumerates; not done here, there is no driver for that dongle.
 */
static void mt5398_phy_init(struct musb *musb, void __iomem *phy)
{
	void __iomem *mbase = musb->mregs;
	u8 v;

	phy_rmw(phy, PHY_CTL0, 0, BIT(14));		/* VRT insert R enable */
	phy_rmw(phy, PHY_RESET, 0, BIT(14));		/* PHY soft reset pulse */
	phy_rmw(phy, PHY_RESET, BIT(14), 0);
	phy_rmw(phy, PHY_OTG, 0x3f3f, 0x3f2c);		/* OTG / ID / VBUS valid settings */
	phy_rmw(phy, PHY_RESET, BIT(18), 0);		/* suspendm control */
	phy_rmw(phy, PHY_CTL7, 0, BIT(24));		/* HS TX current enable (eye diagram) */

	v = readb(mbase + MT_PERF3);
	mt5398_writeb(mbase, MT_PERF3, (v | 0x80) & ~0x40);
	mt5398_writeb(mbase, MT_HUBCFG, readb(mbase + MT_HUBCFG) | 0x04);

	/* fast OTG timers only while the session opens, as stock; cleared again below */
	writel(readl(mbase + MT_TM1) | 1, mbase + MT_TM1);
	mt5398_writeb(mbase, MUSB_DEVCTL, readb(mbase + MUSB_DEVCTL) | MUSB_DEVCTL_SESSION);
	phy_rmw(phy, PHY_SLEW, 0x000000ff, 0x00000011);	/* FS/LS slew rate */
	phy_rmw(phy, PHY_SLEW, 0x00070000, 0x00040000);	/* HS slew rate */
	writel(readl(mbase + MT_TM1) & ~1, mbase + MT_TM1);

	writel(2, mbase + MT_FIFOBYTECNT);
}

static int mt5398_musb_init(struct musb *musb)
{
	struct mt5398_glue *glue = dev_get_drvdata(musb->controller->parent);

	dev_info(glue->dev, "devctl %02x configdata %02x epinfo %02x raminfo %02x l1 %08x/%08x\n",
		 readb(musb->mregs + MUSB_DEVCTL), readb(musb->mregs + 0x10f),
		 readb(musb->mregs + MUSB_EPINFO), readb(musb->mregs + MUSB_RAMINFO),
		 readl(musb->mregs + MT_L1INTS), readl(musb->mregs + MT_L1INTM));

	mt5398_quiesce(musb);
	mt5398_phy_init(musb, glue->phy);

	/* ConfigData (flat copy at 0x10f) reads 0x1f: dynamic FIFOs, whatever the indexed view says */
	musb->dyn_fifo = true;
	musb->isr = mt5398_musb_interrupt;

	writel(MT_L1_TX | MT_L1_RX | MT_L1_USBCOM, musb->mregs + MT_L1INTM);
	return 0;
}

static int mt5398_musb_exit(struct musb *musb)
{
	mt5398_quiesce(musb);
	return 0;
}

static u32 mt5398_busctl_offset(u8 epnum, u16 offset)
{
	return 0x480 + 8 * epnum + offset;
}

static const struct musb_platform_ops mt5398_musb_ops = {
	.quirks		= 0,			/* flat endpoint registers */
	.init		= mt5398_musb_init,
	.exit		= mt5398_musb_exit,
	.busctl_offset	= mt5398_busctl_offset,
	.writeb		= mt5398_writeb,
	.writew		= mt5398_writew,
	.clearb		= mt5398_clearb,
	.clearw		= mt5398_clearw,
	.read_fifo	= mt5398_read_fifo,
	.write_fifo	= mt5398_write_fifo,
	.get_toggle	= mt5398_get_toggle,
	.set_toggle	= mt5398_set_toggle,
};

/*
 * EPINFO = 0x75: 5 TX + 7 RX endpoints besides ep0; RAMINFO = 0x4b: 2^11 words (8 KB) of
 * FIFO RAM, 4 DMA channels. 64 (ep0) + 12 x 512 bytes fits.
 */
static struct musb_fifo_cfg mt5398_fifo_cfg[] = {
	{ .hw_ep_num = 1, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 1, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 5, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 5, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 6, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 7, .style = FIFO_RX, .maxpacket = 512, },
};

static const struct musb_hdrc_config mt5398_musb_config = {
	.multipoint	= true,		/* hubs: per-endpoint function/hub address registers */
	.num_eps	= 8,
	.ram_bits	= 11,
	.fifo_cfg	= mt5398_fifo_cfg,
	.fifo_cfg_size	= ARRAY_SIZE(mt5398_fifo_cfg),
};

static int mt5398_probe(struct platform_device *pdev)
{
	struct musb_hdrc_platform_data pdata = {
		.mode		= MUSB_HOST,
		.config		= &mt5398_musb_config,
		.platform_ops	= &mt5398_musb_ops,
		.power		= 250,	/* x 2 mA */
	};
	struct device *dev = &pdev->dev;
	struct platform_device *musb;
	struct mt5398_glue *glue;
	struct resource *res;
	int ret;

	glue = devm_kzalloc(dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		return -ENOMEM;
	glue->dev = dev;

	/* not requested: the musb child requests the core region (index 0) itself */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	if (!res)
		return dev_err_probe(dev, -EINVAL, "no \"phy\" registers\n");
	glue->phy = devm_ioremap(dev, res->start, resource_size(res));
	if (!glue->phy)
		return -ENOMEM;

	musb = platform_device_alloc("musb-hdrc", PLATFORM_DEVID_AUTO);
	if (!musb)
		return -ENOMEM;

	musb->dev.parent		= dev;
	musb->dev.dma_mask		= &musb->dev.coherent_dma_mask;
	musb->dev.coherent_dma_mask	= DMA_BIT_MASK(32);
	device_set_of_node_from_dev(&musb->dev, dev);
	glue->musb_pdev = musb;
	platform_set_drvdata(pdev, glue);

	ret = platform_device_add_resources(musb, pdev->resource, pdev->num_resources);
	if (ret)
		goto err_put;
	ret = platform_device_add_data(musb, &pdata, sizeof(pdata));
	if (ret)
		goto err_put;
	ret = platform_device_add(musb);
	if (ret)
		goto err_put;
	return 0;

err_put:
	platform_device_put(musb);
	return dev_err_probe(dev, ret, "cannot add the musb device\n");
}

static void mt5398_remove(struct platform_device *pdev)
{
	struct mt5398_glue *glue = platform_get_drvdata(pdev);

	platform_device_unregister(glue->musb_pdev);
}

static const struct of_device_id mt5398_musb_match[] = {
	{ .compatible = "mediatek,mt5398-musb" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt5398_musb_match);

static struct platform_driver mt5398_musb_driver = {
	.probe		= mt5398_probe,
	.remove_new	= mt5398_remove,
	.driver		= {
		.name		= "musb-mt5398",
		.of_match_table	= mt5398_musb_match,
	},
};
module_platform_driver(mt5398_musb_driver);

MODULE_DESCRIPTION("MediaTek MT5398 MUSB glue (host, PIO)");
MODULE_LICENSE("GPL");
