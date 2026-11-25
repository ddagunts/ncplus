// SPDX-License-Identifier: GPL-2.0-only
/*
 * IR remote (and the TV's own buttons) of LG NetCast TVs. The IR receiver is wired to the
 * MICOM, which decodes LG's NEC codes itself and hands over finished key events, so this is
 * a scancode driver: other brands' remotes and raw IR can't be received.
 *
 * Protocol, from the stock system (MICOM_Task() in RELEASE, disassembled): every 40 ms LG
 * writes register 0xc0 and reads 3 bytes in one transfer, {event, code, check}. For keys
 * code + check = 0xff. Events: 2 remote key pressed, 3 remote key repeated, 4/5 the same for
 * the TV's own buttons (same codes). The others are timers, sensors, CEC and so on.
 * The code is the NEC command byte of LG's address 0x04.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mfd/lg-micom.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <media/rc-core.h>

#define MICOM_REG_EVENT		0xc0

#define EV_REMOTE_PRESS		2
#define EV_REMOTE_REPEAT	3
#define EV_LOCAL_PRESS		4
#define EV_LOCAL_REPEAT		5

#define MICOM_CANCEL_POWER_OFF	0x52

#define LG_NEC_ADDR		0x04
#define LG_POWER		0x08

#define FORCE_OFF_PRESSES	3	/* Power presses within FORCE_OFF_MS that force it off */
#define FORCE_OFF_MS		30000

#define POLL_MS			40	/* as LG */
#define POLL_ERR_MS		1000	/* while the MICOM doesn't answer */
#define NEC_REPEAT_MS		110	/* rc-core adds it to the timeout */

/*
 * A key counts as released this long after its last event. Measured on the TV (as polled):
 * the first repeat comes 350-400 ms after the press, later ones at most 100 ms apart. So a
 * press needs a longer time than a repeat, which must be short or a held key overshoots.
 * hold_ms must also stay below the autorepeat delay (500 ms), or a short press repeats.
 */
static unsigned int hold_ms = 450;
module_param(hold_ms, uint, 0644);
MODULE_PARM_DESC(hold_ms, "key release after this many ms without a first repeat (default 450)");

static unsigned int repeat_ms = 250;
module_param(repeat_ms, uint, 0644);
MODULE_PARM_DESC(repeat_ms, "key release after this many ms without a further repeat (default 250)");

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "log every MICOM event that differs from the last one");

struct micom_ir {
	struct device *dev;
	struct lg_micom *micom;
	struct rc_dev *rc;
	struct delayed_work work;
	bool drain;		/* drop the event left over from before open */
	bool failing;
	bool toggle;
	unsigned int presses;	/* of Power, since first_press */
	unsigned long first_press;
	u8 last[3];
};

/*
 * Codes from the key table in RELEASE (all LG remotes). Xorg only gets key codes up to 247,
 * so the keys useful on a desktop are ones it has: Back = Esc, CH+/- = Page Up/Down,
 * the colour keys = F1-F4. "ir-keytable" can change all of it at run time.
 *
 * Power is KEY_POWER: logind shuts the system down. See micom_ir_power().
 */
#define LG(code)	RC_SCANCODE_NEC(LG_NEC_ADDR, code)

static struct rc_map_table lg_netcast[] = {
	{ LG(0x40), KEY_UP },
	{ LG(0x41), KEY_DOWN },
	{ LG(0x07), KEY_LEFT },
	{ LG(0x06), KEY_RIGHT },
	{ LG(0x44), KEY_ENTER },		/* OK */
	{ LG(0x28), KEY_ESC },			/* Back */
	{ LG(0x5b), KEY_EXIT },
	{ LG(0x7c), KEY_HOMEPAGE },		/* Home / Smart */
	{ LG(0x43), KEY_MENU },			/* Settings */
	{ LG(0x45), KEY_COMPOSE },		/* Q.Menu: context menu */
	{ LG(0x1a), KEY_TAB },			/* Q.View / Flashback */

	{ LG(LG_POWER), KEY_POWER },
	{ LG(0x0b), KEY_VIDEO_NEXT },		/* Input */
	{ LG(0x02), KEY_VOLUMEUP },
	{ LG(0x03), KEY_VOLUMEDOWN },
	{ LG(0x09), KEY_MUTE },
	{ LG(0x00), KEY_PAGEUP },		/* CH + */
	{ LG(0x01), KEY_PAGEDOWN },		/* CH - */

	{ LG(0x10), KEY_0 },
	{ LG(0x11), KEY_1 },
	{ LG(0x12), KEY_2 },
	{ LG(0x13), KEY_3 },
	{ LG(0x14), KEY_4 },
	{ LG(0x15), KEY_5 },
	{ LG(0x16), KEY_6 },
	{ LG(0x17), KEY_7 },
	{ LG(0x18), KEY_8 },
	{ LG(0x19), KEY_9 },
	{ LG(0x4c), KEY_MINUS },		/* - / Dash */

	{ LG(0x72), KEY_F1 },			/* Red */
	{ LG(0x71), KEY_F2 },			/* Green */
	{ LG(0x63), KEY_F3 },			/* Yellow */
	{ LG(0x61), KEY_F4 },			/* Blue */

	{ LG(0xb0), KEY_PLAY },
	{ LG(0xba), KEY_PAUSE },
	{ LG(0xb1), KEY_STOP },
	{ LG(0x8f), KEY_REWIND },
	{ LG(0x8e), KEY_FASTFORWARD },
	{ LG(0xbd), KEY_RECORD },

	{ LG(0x0f), KEY_TV },
	{ LG(0x1e), KEY_FAVORITES },
	{ LG(0xaa), KEY_INFO },
	{ LG(0xa9), KEY_PROGRAM },		/* Guide */
	{ LG(0xab), KEY_PROGRAM },		/* Guide */
};

static struct rc_map_list lg_netcast_map = {
	.map = {
		.scan = lg_netcast,
		.size = ARRAY_SIZE(lg_netcast),
		.rc_proto = RC_PROTO_NEC,
		.name = "rc-lg-netcast",
	}
};

static int micom_ir_read(struct micom_ir *ir, u8 ev[3])
{
	struct i2c_client *c = ir->micom->client;
	u8 reg = MICOM_REG_EVENT;
	struct i2c_msg msgs[] = {
		{ .addr = c->addr, .len = 1, .buf = &reg },
		{ .addr = c->addr, .len = 3, .buf = ev, .flags = I2C_M_RD },
	};
	int ret;

	mutex_lock(&ir->micom->lock);
	ret = i2c_transfer(c->adapter, msgs, ARRAY_SIZE(msgs));
	mutex_unlock(&ir->micom->lock);
	if (ret < 0)
		return ret;
	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

/*
 * On Power the MICOM starts a timer and switches the TV off when it runs out, shut down or
 * not. LG cancels that (DDI_MICOM_CancelPowerOff()) whenever it wants to stay on, so do the
 * same and leave the power-off to the end of the shutdown (lg-micom). If Linux is stuck,
 * repeated presses still work: the third one within 30 s is not cancelled.
 */
static void micom_ir_power(struct micom_ir *ir, bool press)
{
	u8 cmd = MICOM_CANCEL_POWER_OFF;
	int tries, ret = -EIO;

	if (press) {
		if (!ir->presses ||
		    time_after(jiffies, ir->first_press + msecs_to_jiffies(FORCE_OFF_MS))) {
			ir->presses = 0;
			ir->first_press = jiffies;
		}
		ir->presses++;
	}
	if (ir->presses >= FORCE_OFF_PRESSES) {
		if (press)
			dev_warn(ir->dev, "Power pressed %u times: not cancelling the power-off\n",
				 ir->presses);
		return;
	}

	mutex_lock(&ir->micom->lock);
	for (tries = 0; tries < 3; tries++) {
		ret = i2c_master_send(ir->micom->client, &cmd, 1);
		if (ret == 1)
			break;
		msleep(20);
	}
	mutex_unlock(&ir->micom->lock);
	if (ret != 1)
		dev_err(ir->dev, "cancelling the power-off failed: %d\n", ret);
	else if (press)
		dev_info(ir->dev, "Power: the MICOM's power-off is cancelled\n");
}

static void micom_ir_event(struct micom_ir *ir, const u8 ev[3])
{
	bool press = ev[0] == EV_REMOTE_PRESS || ev[0] == EV_LOCAL_PRESS;
	unsigned int ms = press ? hold_ms : repeat_ms;

	switch (ev[0]) {
	case EV_REMOTE_PRESS:
	case EV_LOCAL_PRESS:
	case EV_REMOTE_REPEAT:
	case EV_LOCAL_REPEAT:
		break;
	default:
		return;
	}
	if (ev[1] + ev[2] != 0xff)
		return;
	if (ev[1] == LG_POWER)
		micom_ir_power(ir, press);
	/* a new press of the same key must not look like the old one still held */
	if (press)
		ir->toggle = !ir->toggle;
	ir->rc->timeout = MS_TO_US(max(ms, NEC_REPEAT_MS + 1u) - NEC_REPEAT_MS);
	rc_keydown(ir->rc, RC_PROTO_NEC, LG(ev[1]), ir->toggle);
}

static void micom_ir_work(struct work_struct *work)
{
	struct micom_ir *ir = container_of(work, struct micom_ir, work.work);
	unsigned int next = POLL_MS;
	u8 ev[3];
	int ret;

	ret = micom_ir_read(ir, ev);
	if (ret) {
		if (!ir->failing)
			dev_warn(ir->dev, "MICOM doesn't answer: %d\n", ret);
		ir->failing = true;
		next = POLL_ERR_MS;
	} else {
		if (ir->failing)
			dev_info(ir->dev, "MICOM answers again\n");
		ir->failing = false;
		if (debug && memcmp(ev, ir->last, sizeof(ev)))
			dev_info(ir->dev, "event %3ph\n", ev);
		memcpy(ir->last, ev, sizeof(ev));
		if (!ir->drain)
			micom_ir_event(ir, ev);
		ir->drain = false;
	}
	schedule_delayed_work(&ir->work, msecs_to_jiffies(next));
}

/* The bus must be free when the power-off command is sent. */
static void micom_ir_shutdown(struct platform_device *pdev)
{
	struct micom_ir *ir = platform_get_drvdata(pdev);

	disable_delayed_work_sync(&ir->work);
}

static int micom_ir_open(struct rc_dev *rc)
{
	struct micom_ir *ir = rc->priv;

	ir->drain = true;
	schedule_delayed_work(&ir->work, 0);
	return 0;
}

static void micom_ir_close(struct rc_dev *rc)
{
	struct micom_ir *ir = rc->priv;

	cancel_delayed_work_sync(&ir->work);
}

static int micom_ir_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct micom_ir *ir;
	struct rc_dev *rc;
	u8 ev[3];
	int ret;

	ir = devm_kzalloc(dev, sizeof(*ir), GFP_KERNEL);
	if (!ir)
		return -ENOMEM;
	ir->dev = dev;
	ir->micom = dev_get_drvdata(dev->parent);
	platform_set_drvdata(pdev, ir);
	INIT_DELAYED_WORK(&ir->work, micom_ir_work);

	rc = devm_rc_allocate_device(dev, RC_DRIVER_SCANCODE);
	if (!rc)
		return -ENOMEM;
	ir->rc = rc;
	rc->priv = ir;
	rc->device_name = "LG NetCast remote";
	rc->driver_name = "lg-micom-ir";
	rc->input_phys = "lg-micom/input0";
	rc->input_id.bustype = BUS_I2C;
	rc->map_name = "rc-lg-netcast";
	rc->allowed_protocols = RC_PROTO_BIT_NEC;
	rc->open = micom_ir_open;
	rc->close = micom_ir_close;

	/* not fatal: the poll keeps trying, and the log says what is wrong */
	ret = micom_ir_read(ir, ev);
	if (ret)
		dev_warn(dev, "first read failed: %d\n", ret);
	else
		dev_info(dev, "first read: %3ph\n", ev);

	return devm_rc_register_device(dev, rc);
}

static const struct of_device_id micom_ir_of_match[] = {
	{ .compatible = "lg,netcast-micom-ir" },
	{ }
};
MODULE_DEVICE_TABLE(of, micom_ir_of_match);

static struct platform_driver micom_ir_driver = {
	.probe = micom_ir_probe,
	.shutdown = micom_ir_shutdown,
	.driver = {
		.name = "lg-micom-ir",
		.of_match_table = micom_ir_of_match,
	},
};

static int __init micom_ir_init(void)
{
	int ret = rc_map_register(&lg_netcast_map);

	if (ret)
		return ret;
	ret = platform_driver_register(&micom_ir_driver);
	if (ret)
		rc_map_unregister(&lg_netcast_map);
	return ret;
}
module_init(micom_ir_init);

static void __exit micom_ir_exit(void)
{
	platform_driver_unregister(&micom_ir_driver);
	rc_map_unregister(&lg_netcast_map);
}
module_exit(micom_ir_exit);

MODULE_DESCRIPTION("LG NetCast IR remote (MICOM)");
MODULE_LICENSE("GPL");
