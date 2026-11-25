// SPDX-License-Identifier: GPL-2.0-only
/*
 * Panel backlight of LG NetCast TVs, switched by the MICOM (LG's standby/power
 * microcontroller) over I2C.
 *
 * LG's path (stock RELEASE, confirmed with a live capture; src/apps/ncscreen.c):
 * API_DDM_BackLight_OnOff() -> MICOM GPIO 0x2102 -> an I2C write of {0x36, 0, 0} (on) or
 * {0x37, 0, 0} (off) to the MICOM on MediaTek SIF bus 2, 8-bit address 0x52, then 10 ms
 * and a 1-byte read of the MICOM's answer, up to 3 tries. The answer is not reliable from
 * outside LG's own process (it reads 0xff while the command works), so it is only logged.
 *
 * Only on/off: max_brightness 1. The DRM driver blanks through this device (DPMS), and
 * userspace can use bl_power or brightness directly. Nothing is sent at probe: the stock
 * kernel left the backlight on.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mfd/lg-micom.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define MICOM_BL_ON	0x36
#define MICOM_BL_OFF	0x37

struct micom_bl {
	struct device *dev;
	struct lg_micom *micom;
	struct mutex lock;
	bool on;
};

static int micom_bl_set(struct micom_bl *m, bool on)
{
	u8 cmd[3] = { on ? MICOM_BL_ON : MICOM_BL_OFF, 0, 0 };
	struct i2c_client *client = m->micom->client;
	int tries, ret = -EIO;
	u8 ack = 0xff;

	/* the IR poll must not come between the command and its answer */
	mutex_lock(&m->micom->lock);
	for (tries = 0; tries < 3; tries++) {
		ret = i2c_master_send(client, cmd, sizeof(cmd));
		if (ret == sizeof(cmd)) {
			msleep(10);
			if (i2c_master_recv(client, &ack, 1) != 1)
				ack = 0xff;
			mutex_unlock(&m->micom->lock);
			dev_dbg(m->dev, "backlight %s, answer %02x\n", on ? "on" : "off", ack);
			return 0;
		}
		msleep(10);
	}
	mutex_unlock(&m->micom->lock);
	dev_err(m->dev, "backlight %s failed: %d\n", on ? "on" : "off", ret);
	return ret < 0 ? ret : -EIO;
}

static int micom_bl_update_status(struct backlight_device *bd)
{
	struct micom_bl *m = bl_get_data(bd);
	bool on = backlight_get_brightness(bd) > 0;
	int ret = 0;

	mutex_lock(&m->lock);
	if (on != m->on) {
		ret = micom_bl_set(m, on);
		if (!ret)
			m->on = on;
	}
	mutex_unlock(&m->lock);
	return ret;
}

static const struct backlight_ops micom_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = micom_bl_update_status,
};

static int micom_bl_probe(struct platform_device *pdev)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 1,
		.brightness = 1,
		.power = BACKLIGHT_POWER_ON,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};
	struct backlight_device *bd;
	struct micom_bl *m;

	m = devm_kzalloc(&pdev->dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->dev = &pdev->dev;
	m->micom = dev_get_drvdata(pdev->dev.parent);
	m->on = true;
	mutex_init(&m->lock);

	bd = devm_backlight_device_register(&pdev->dev, "micom", &pdev->dev, m,
					    &micom_bl_ops, &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);
	platform_set_drvdata(pdev, bd);
	return 0;
}

/* Leave the panel lit for whatever runs next (the stock system after a watchdog reset). */
static void micom_bl_shutdown(struct platform_device *pdev)
{
	struct backlight_device *bd = platform_get_drvdata(pdev);
	struct micom_bl *m = bl_get_data(bd);

	if (system_state == SYSTEM_POWER_OFF)
		return;
	mutex_lock(&m->lock);
	if (!m->on && !micom_bl_set(m, true))
		m->on = true;
	mutex_unlock(&m->lock);
}

static const struct of_device_id micom_bl_of_match[] = {
	{ .compatible = "lg,netcast-micom-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, micom_bl_of_match);

static struct platform_driver micom_bl_driver = {
	.driver = {
		.name = "lg-micom-bl",
		.of_match_table = micom_bl_of_match,
	},
	.probe = micom_bl_probe,
	.shutdown = micom_bl_shutdown,
};
module_platform_driver(micom_bl_driver);

MODULE_DESCRIPTION("LG NetCast MICOM panel backlight");
MODULE_LICENSE("GPL");
