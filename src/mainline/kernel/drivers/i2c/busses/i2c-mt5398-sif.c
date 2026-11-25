// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C master for the SIF ("serial interface") engine of the MediaTek MT5398 TV SoC
 * (LG NetCast), the one LG's GPL U-Boot drives as SIF engine V2 (board/mt5398/module).
 * Where that U-Boot and the TV's own kernel module (dtv_driver_adpt.ko, disassembled)
 * disagree - pad mux, deglitch, clock stretching - this follows the module.
 *
 * The engine is byte-command driven: CTRL1 takes one command (start, write up to 8 bytes,
 * read up to 8 bytes with ACK or with a final NACK, stop), TRI starts it and reads back 1
 * while busy, and the ACK field reports one bit per byte the slave acknowledged. Data goes
 * through D0/D1, LSB first. MediaTek's later MT7621 I2C block has the same command layout.
 *
 * MediaTek's SIF bus 0 (system), 1 (tuner) and 2 (second) all use this one master; what
 * tells them apart is the pin mux in CKGEN +0x600/+0x604 (the U-Boot source says
 * +0x400/+0x404), which LG sets before every transfer. "mediatek,sif-bus" picks the
 * pads; this driver sets them at probe and before each transfer, since the stock kernel may
 * have left another bus selected. LG NetCast's MICOM (power, backlight, IR) is on bus 2, the
 * osda1/oscl1 pads. Beware: LG's /dev/mtal ioctls call that one bus 1, the module swaps 1
 * and 2 before it calls the SIF driver.
 *
 * LG runs the master with clock stretching and deglitch level 3 for every device. With
 * stretching on, the divider is reduced by 6 (2 x the deglitch level) for the same rate.
 *
 * Polled: TRI is short-lived (an 8-byte command takes ~0.8 ms at 100 kHz).
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define SIF_CTRL0		0x00
#define  CTRL0_ODRAIN_OFF	BIT(31)		/* 1 = push-pull */
#define  CTRL0_CLK_DIV		GENMASK(27, 16)	/* SCL = 27 MHz / div */
#define  CTRL0_DEGLITCH		GENMASK(15, 8)
#define  CTRL0_DEGLITCH_EN	BIT(5)
#define  DEGLITCH_LEVEL	3
#define  CTRL0_SCL		BIT(3)		/* line states, read only */
#define  CTRL0_SDA		BIT(2)
#define  CTRL0_EN		BIT(1)
#define  CTRL0_STRETCH		BIT(0)
#define SIF_CTRL1		0x04
#define  CTRL1_ACK		GENMASK(23, 16)
#define  CTRL1_PGLEN		GENMASK(10, 8)	/* bytes - 1 */
#define  CTRL1_MODE		GENMASK(6, 4)
#define  CTRL1_TRI		BIT(0)
#define SIF_D0			0x10
#define SIF_D1			0x14

#define MODE_START		1
#define MODE_WRITE		2
#define MODE_STOP		3
#define MODE_READ_LAST		4		/* NACK after the last byte */
#define MODE_READ		5

#define SIF_REF_HZ		27000000
#define SIF_PAGE		8

struct sif {
	struct i2c_adapter adap;
	struct device *dev;
	void __iomem *base;
	void __iomem *pinmux;		/* CKGEN +0x600, +0x604 */
	u32 bus;
	u32 div;
	bool atomic;			/* power-off: no sleeping */
};

/* SIF_MtkCfgPinSetFunc() of the stock module for buses 0..2. */
static void sif_pinmux(struct sif *s)
{
	u32 a = readl(s->pinmux), b = readl(s->pinmux + 4);

	switch (s->bus) {
	case 0:		/* osda0/oscl0: a[19:18] = 1, a[21:20] != 2, b[1:0] != 1 */
		if ((a & GENMASK(21, 20)) == 0x00200000)
			a &= ~GENMASK(21, 20);
		a = (a & ~GENMASK(19, 18)) | 0x00040000;
		if ((b & GENMASK(1, 0)) == 1)
			b &= ~GENMASK(1, 0);
		break;
	case 1:		/* osda2/oscl2: b[1:0] = 1, a[21:20] != 2, a[19:18] != 1 */
		if ((a & GENMASK(21, 20)) == 0x00200000)
			a &= ~GENMASK(21, 20);
		if ((a & GENMASK(19, 18)) == 0x00040000)
			a &= ~GENMASK(19, 18);
		b = (b & ~GENMASK(1, 0)) | 1;
		break;
	case 2:		/* osda1/oscl1: a[21:20] = 2, a[19:18] != 1, b[1:0] != 1 */
		a = (a & ~GENMASK(21, 20)) | 0x00200000;
		if ((a & GENMASK(19, 18)) == 0x00040000)
			a &= ~GENMASK(19, 18);
		if ((b & GENMASK(1, 0)) == 1)
			b &= ~GENMASK(1, 0);
		break;
	}
	writel(a, s->pinmux);
	writel(b, s->pinmux + 4);
}

/* Run one command; returns the ACK field or a negative errno. */
static int sif_cmd(struct sif *s, unsigned int mode, unsigned int len)
{
	u32 v = readl(s->base + SIF_CTRL1);
	int ret;

	v &= ~(CTRL1_MODE | CTRL1_PGLEN | CTRL1_TRI);
	v |= FIELD_PREP(CTRL1_MODE, mode) | CTRL1_TRI;
	if (len)
		v |= FIELD_PREP(CTRL1_PGLEN, len - 1);
	writel(v, s->base + SIF_CTRL1);
	if (s->atomic)
		ret = readl_poll_timeout_atomic(s->base + SIF_CTRL1, v, !(v & CTRL1_TRI), 10, 100000);
	else
		ret = readl_poll_timeout(s->base + SIF_CTRL1, v, !(v & CTRL1_TRI), 10, 100000);
	if (ret) {
		dev_err_ratelimited(s->dev, "command %u timed out (ctrl0 %08x ctrl1 %08x)\n", mode,
				    readl(s->base + SIF_CTRL0), v);
		return ret;
	}
	return FIELD_GET(CTRL1_ACK, v);
}

static void sif_setup(struct sif *s)
{
	u32 v = readl(s->base + SIF_CTRL0);

	/* as the stock module: open drain, clock stretching, deglitch on */
	v &= ~(CTRL0_CLK_DIV | CTRL0_DEGLITCH | CTRL0_ODRAIN_OFF);
	v |= FIELD_PREP(CTRL0_CLK_DIV, s->div - 2 * DEGLITCH_LEVEL) |
	     FIELD_PREP(CTRL0_DEGLITCH, DEGLITCH_LEVEL) |
	     CTRL0_DEGLITCH_EN | CTRL0_STRETCH | CTRL0_EN;
	writel(v, s->base + SIF_CTRL0);
	sif_pinmux(s);
}

static int sif_write(struct sif *s, const u8 *buf, unsigned int len)
{
	while (len) {
		unsigned int n = min(len, SIF_PAGE), i;
		u32 d[2] = { 0, 0 };
		int ack;

		for (i = 0; i < n; i++)
			d[i / 4] |= buf[i] << (8 * (i % 4));
		writel(d[0], s->base + SIF_D0);
		writel(d[1], s->base + SIF_D1);
		ack = sif_cmd(s, MODE_WRITE, n);
		if (ack < 0)
			return ack;
		if (ack != GENMASK(n - 1, 0))
			return -ENXIO;
		buf += n;
		len -= n;
	}
	return 0;
}

static int sif_read(struct sif *s, u8 *buf, unsigned int len)
{
	while (len) {
		unsigned int n = min(len, SIF_PAGE), i;
		u32 d[2];
		int ret;

		ret = sif_cmd(s, n == len ? MODE_READ_LAST : MODE_READ, n);
		if (ret < 0)
			return ret;
		d[0] = readl(s->base + SIF_D0);
		d[1] = readl(s->base + SIF_D1);
		for (i = 0; i < n; i++)
			buf[i] = d[i / 4] >> (8 * (i % 4));
		buf += n;
		len -= n;
	}
	return 0;
}

static int sif_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct sif *s = i2c_get_adapdata(adap);
	int i, ret = 0;
	u32 v;

	sif_setup(s);
	v = readl(s->base + SIF_CTRL0);
	if (!(v & CTRL0_SCL) || !(v & CTRL0_SDA)) {
		sif_cmd(s, MODE_STOP, 0);	/* as LG: try to release the bus first */
		v = readl(s->base + SIF_CTRL0);
		if (!(v & CTRL0_SCL) || !(v & CTRL0_SDA))
			return -EBUSY;
	}

	for (i = 0; i < num; i++) {
		struct i2c_msg *m = &msgs[i];
		u8 addr = i2c_8bit_addr_from_msg(m);

		if (m->flags & I2C_M_TEN) {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = sif_cmd(s, MODE_START, 0);
		if (ret < 0)
			break;
		ret = sif_write(s, &addr, 1);
		if (ret)
			break;
		ret = (m->flags & I2C_M_RD) ? sif_read(s, m->buf, m->len) : sif_write(s, m->buf, m->len);
		if (ret)
			break;
	}
	sif_cmd(s, MODE_STOP, 0);
	return ret ? ret : num;
}

/* Only for the last commands before power-off: interrupts off, nothing else running. */
static int sif_xfer_atomic(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct sif *s = i2c_get_adapdata(adap);
	int ret;

	s->atomic = true;
	ret = sif_xfer(adap, msgs, num);
	s->atomic = false;
	return ret;
}

static u32 sif_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
}

static const struct i2c_algorithm sif_algo = {
	.xfer = sif_xfer,
	.xfer_atomic = sif_xfer_atomic,
	.functionality = sif_func,
};

static const struct i2c_adapter_quirks sif_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN,
};

static int sif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sif *s;
	u32 hz = 100000;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;
	s->base = devm_platform_ioremap_resource_byname(pdev, "sif");
	if (IS_ERR(s->base))
		return PTR_ERR(s->base);
	s->pinmux = devm_platform_ioremap_resource_byname(pdev, "pinmux");
	if (IS_ERR(s->pinmux))
		return PTR_ERR(s->pinmux);
	if (of_property_read_u32(dev->of_node, "mediatek,sif-bus", &s->bus) || s->bus > 2)
		return dev_err_probe(dev, -EINVAL, "mediatek,sif-bus must be 0, 1 or 2\n");
	of_property_read_u32(dev->of_node, "clock-frequency", &hz);
	s->div = clamp(DIV_ROUND_UP(SIF_REF_HZ, hz ? hz : 1), 2u * DEGLITCH_LEVEL + 2,
		       (u32)FIELD_MAX(CTRL0_CLK_DIV));

	dev_info(dev, "bus %u, div %u (%u Hz); as left: ctrl0 %08x ctrl1 %08x pinmux %08x %08x\n",
		 s->bus, s->div, SIF_REF_HZ / s->div, readl(s->base + SIF_CTRL0),
		 readl(s->base + SIF_CTRL1), readl(s->pinmux), readl(s->pinmux + 4));

	s->adap.owner = THIS_MODULE;
	s->adap.algo = &sif_algo;
	s->adap.quirks = &sif_quirks;
	s->adap.retries = 1;
	s->adap.dev.parent = dev;
	s->adap.dev.of_node = dev->of_node;
	snprintf(s->adap.name, sizeof(s->adap.name), "MT5398 SIF bus %u", s->bus);
	i2c_set_adapdata(&s->adap, s);
	platform_set_drvdata(pdev, s);
	sif_setup(s);

	return devm_i2c_add_adapter(dev, &s->adap);
}

static const struct of_device_id sif_of_match[] = {
	{ .compatible = "mediatek,mt5398-sif" },
	{ }
};
MODULE_DEVICE_TABLE(of, sif_of_match);

static struct platform_driver sif_driver = {
	.probe = sif_probe,
	.driver = {
		.name = "mt5398-sif",
		.of_match_table = sif_of_match,
	},
};
module_platform_driver(sif_driver);

MODULE_DESCRIPTION("MediaTek MT5398 SIF I2C master");
MODULE_LICENSE("GPL");
