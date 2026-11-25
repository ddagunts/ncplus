// SPDX-License-Identifier: GPL-2.0-only
/*
 * The MICOM of LG NetCast TVs: the standby/power microcontroller, an I2C slave of the SoC
 * (8-bit address 0x52). It also switches the panel backlight and decodes the IR remote.
 * The functions are child nodes in the DT (lg-micom-bl, lg-micom-ir).
 *
 * This part powers the TV off: LG's shutdown (API_DDM_POWER_ShutdownSystem() in RELEASE,
 * disassembled) ends with the "cold standby" command {0x25, 0, reason}. The TV is then in
 * standby and starts LG's system on the next Power key.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mfd/lg-micom.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/reboot.h>

#define MICOM_POWER_CONTROL	0x25
#define  POWER_COLD_STANDBY	0x00
#define  REASON_REMOTE_KEY	0x10

#define POWER_OFF_TRIES		3
#define POWER_OFF_WAIT_MS	10000

/* Interrupts are off and the other CPU is stopped: no locks, the I2C transfer is atomic. */
static int lg_micom_power_off(struct sys_off_data *data)
{
	struct lg_micom *m = data->cb_data;
	u8 cmd[3] = { MICOM_POWER_CONTROL, POWER_COLD_STANDBY, REASON_REMOTE_KEY };
	int tries, ret = -EIO;

	for (tries = 0; tries < POWER_OFF_TRIES; tries++) {
		ret = i2c_master_send(m->client, cmd, sizeof(cmd));
		if (ret == sizeof(cmd))
			break;
		mdelay(10);
	}
	mdelay(POWER_OFF_WAIT_MS);

	/* Without a console a dead screen is worse than LG's system. */
	dev_emerg(&m->client->dev, "still powered (command: %d), restarting\n", ret);
	emergency_restart();
	return NOTIFY_DONE;
}

static int lg_micom_probe(struct i2c_client *client)
{
	struct lg_micom *m;
	int ret;

	m = devm_kzalloc(&client->dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->client = client;
	mutex_init(&m->lock);
	i2c_set_clientdata(client, m);

	ret = devm_register_sys_off_handler(&client->dev, SYS_OFF_MODE_POWER_OFF,
					    SYS_OFF_PRIO_DEFAULT, lg_micom_power_off, m);
	if (ret)
		return ret;
	return devm_of_platform_populate(&client->dev);
}

static const struct of_device_id lg_micom_of_match[] = {
	{ .compatible = "lg,netcast-micom" },
	{ }
};
MODULE_DEVICE_TABLE(of, lg_micom_of_match);

static struct i2c_driver lg_micom_driver = {
	.driver = {
		.name = "lg-micom",
		.of_match_table = lg_micom_of_match,
	},
	.probe = lg_micom_probe,
};
module_i2c_driver(lg_micom_driver);

MODULE_DESCRIPTION("LG NetCast MICOM");
MODULE_LICENSE("GPL");
