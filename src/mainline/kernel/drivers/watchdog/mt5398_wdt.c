// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT5398 PDWNC watchdog 0 (LG NetCast TVs).
 *
 * Two registers, shared with the syscon-reboot restart nodes through a syscon regmap:
 *   +0  control: bit 0 enables watchdog 0 (bit 24 reads 1 after a watchdog reset)
 *   +4  counter: counts UP at 27 MHz while enabled and resets the SoC when it overflows
 * A ping reloads the counter with 2^32 - timeout ticks; LG's own driver kicks it the same
 * way, writing the counter while the watchdog runs. Full range is ~159 s.
 *
 * The stock kernel leaves the watchdog off. Userspace on the mainline test boots arms it
 * (open /dev/watchdog, close without the magic 'V') before handing over to an init that
 * may hang, so a stuck boot ends in a reset to the stock software instead of a TV that
 * needs a power cycle.
 */
#include <linux/bits.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/watchdog.h>

#define WDT_CTRL	0x0
#define WDT_CTRL_EN	BIT(0)
#define WDT_CTRL_RST	BIT(24)		/* last reset came from the watchdog */
#define WDT_COUNT	0x4

#define WDT_RATE	27000000U
#define WDT_MAX_MS	(U32_MAX / (WDT_RATE / 1000))
#define WDT_DEFAULT_S	60

struct mt5398_wdt {
	struct watchdog_device wdd;
	struct regmap *map;
	u32 base;	/* offset of the control register inside the regmap */
};

static int mt5398_wdt_ping(struct watchdog_device *wdd)
{
	struct mt5398_wdt *w = watchdog_get_drvdata(wdd);
	u32 ms = min_t(u32, wdd->timeout * 1000, WDT_MAX_MS);

	return regmap_write(w->map, w->base + WDT_COUNT, 0U - ms * (WDT_RATE / 1000));
}

static int mt5398_wdt_start(struct watchdog_device *wdd)
{
	struct mt5398_wdt *w = watchdog_get_drvdata(wdd);
	int ret = mt5398_wdt_ping(wdd);

	return ret ?: regmap_write(w->map, w->base + WDT_CTRL, WDT_CTRL_EN);
}

static int mt5398_wdt_stop(struct watchdog_device *wdd)
{
	struct mt5398_wdt *w = watchdog_get_drvdata(wdd);

	return regmap_write(w->map, w->base + WDT_CTRL, 0);
}

static int mt5398_wdt_set_timeout(struct watchdog_device *wdd, unsigned int t)
{
	wdd->timeout = t;
	return watchdog_active(wdd) ? mt5398_wdt_ping(wdd) : 0;
}

static unsigned int mt5398_wdt_get_timeleft(struct watchdog_device *wdd)
{
	struct mt5398_wdt *w = watchdog_get_drvdata(wdd);
	u32 cnt;

	if (regmap_read(w->map, w->base + WDT_COUNT, &cnt))
		return 0;
	return (0U - cnt) / WDT_RATE;
}

static const struct watchdog_info mt5398_wdt_info = {
	.identity = "MT5398 PDWNC watchdog",
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE |
		   WDIOF_CARDRESET,
};

static const struct watchdog_ops mt5398_wdt_ops = {
	.owner = THIS_MODULE,
	.start = mt5398_wdt_start,
	.stop = mt5398_wdt_stop,
	.ping = mt5398_wdt_ping,
	.set_timeout = mt5398_wdt_set_timeout,
	.get_timeleft = mt5398_wdt_get_timeleft,
};

static int mt5398_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt5398_wdt *w;
	u32 ctrl;
	int ret;

	w = devm_kzalloc(dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->map = syscon_regmap_lookup_by_phandle(dev->of_node, "regmap");
	if (IS_ERR(w->map))
		return dev_err_probe(dev, PTR_ERR(w->map), "no regmap\n");
	of_property_read_u32(dev->of_node, "offset", &w->base);

	ret = regmap_read(w->map, w->base + WDT_CTRL, &ctrl);
	if (ret)
		return ret;

	w->wdd.info = &mt5398_wdt_info;
	w->wdd.ops = &mt5398_wdt_ops;
	w->wdd.parent = dev;
	w->wdd.min_timeout = 1;
	w->wdd.max_hw_heartbeat_ms = WDT_MAX_MS;
	w->wdd.timeout = WDT_DEFAULT_S;
	w->wdd.bootstatus = (ctrl & WDT_CTRL_RST) ? WDIOF_CARDRESET : 0;
	watchdog_init_timeout(&w->wdd, 0, dev);
	watchdog_set_drvdata(&w->wdd, w);
	watchdog_stop_on_reboot(&w->wdd);

	if (ctrl & WDT_CTRL_EN) {
		/* already running (not the case after the stock kernel): keep it fed */
		mt5398_wdt_ping(&w->wdd);
		set_bit(WDOG_HW_RUNNING, &w->wdd.status);
	}

	ret = devm_watchdog_register_device(dev, &w->wdd);
	if (ret)
		return ret;
	dev_info(dev, "timeout %u s (max %u s), ctrl 0x%08x%s\n", w->wdd.timeout,
		 WDT_MAX_MS / 1000, ctrl, (ctrl & WDT_CTRL_RST) ? " (last reset: watchdog)" : "");
	return 0;
}

static const struct of_device_id mt5398_wdt_of_match[] = {
	{ .compatible = "mediatek,mt5398-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt5398_wdt_of_match);

static struct platform_driver mt5398_wdt_driver = {
	.probe = mt5398_wdt_probe,
	.driver = {
		.name = "mt5398-wdt",
		.of_match_table = mt5398_wdt_of_match,
	},
};
module_platform_driver(mt5398_wdt_driver);

MODULE_DESCRIPTION("MediaTek MT5398 PDWNC watchdog");
MODULE_LICENSE("GPL");
