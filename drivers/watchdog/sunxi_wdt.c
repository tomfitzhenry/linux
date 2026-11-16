// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *      sunxi Watchdog Driver
 *
 *      Copyright (c) 2013 Carlo Caione
 *                    2012 Henrik Nordstrom
 *
 *      Based on xen_wdt.c
 *      (c) Copyright 2010 Novell, Inc.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/panic.h>
#include <linux/panic_notifier.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/stringify.h>
#include <linux/watchdog.h>

#define WDT_MAX_TIMEOUT         16
#define WDT_MIN_TIMEOUT         1
#define WDT_TIMEOUT_MASK        0x0F

#define WDT_CTRL_RELOAD         ((1 << 0) | (0x0a57 << 1))

#define WDT_MODE_EN             (1 << 0)

#define DRV_NAME		"sunxi-wdt"
#define DRV_VERSION		"1.0"

static bool nowayout = WATCHDOG_NOWAYOUT;
static unsigned int timeout;

/*
 * This structure stores the register offsets for different variants
 * of Allwinner's watchdog hardware.
 */
struct sunxi_wdt_reg {
	u8 wdt_ctrl;
	u8 wdt_cfg;
	u8 wdt_mode;
	u8 wdt_timeout_shift;
	u8 wdt_reset_mask;
	u8 wdt_reset_val;
	u32 wdt_key_val;
};

struct sunxi_wdt_dev {
	struct watchdog_device wdt_dev;
	void __iomem *wdt_base;
	const struct sunxi_wdt_reg *wdt_regs;
	struct notifier_block panic_nb;
};

/*
 * wdt_timeout_map maps the watchdog timer interval value in seconds to
 * the value of the register WDT_MODE at bits .wdt_timeout_shift ~ +3
 *
 * [timeout seconds] = register value
 *
 */

static const int wdt_timeout_map[] = {
	[1] = 0x1,  /* 1s  */
	[2] = 0x2,  /* 2s  */
	[3] = 0x3,  /* 3s  */
	[4] = 0x4,  /* 4s  */
	[5] = 0x5,  /* 5s  */
	[6] = 0x6,  /* 6s  */
	[8] = 0x7,  /* 8s  */
	[10] = 0x8, /* 10s */
	[12] = 0x9, /* 12s */
	[14] = 0xA, /* 14s */
	[16] = 0xB, /* 16s */
};


static int sunxi_wdt_restart(struct watchdog_device *wdt_dev,
			     unsigned long action, void *data)
{
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;
	u32 val;

	/* Set system reset function */
	val = readl(wdt_base + regs->wdt_cfg);
	val &= ~(regs->wdt_reset_mask);
	val |= regs->wdt_reset_val;
	val |= regs->wdt_key_val;
	writel(val, wdt_base + regs->wdt_cfg);

	/* Set lowest timeout and enable watchdog */
	val = readl(wdt_base + regs->wdt_mode);
	val &= ~(WDT_TIMEOUT_MASK << regs->wdt_timeout_shift);
	val |= WDT_MODE_EN;
	val |= regs->wdt_key_val;
	writel(val, wdt_base + regs->wdt_mode);

	/*
	 * Restart the watchdog. The default (and lowest) interval
	 * value for the watchdog is 0.5s.
	 */
	writel(WDT_CTRL_RELOAD, wdt_base + regs->wdt_ctrl);

	while (1) {
		mdelay(5);
		val = readl(wdt_base + regs->wdt_mode);
		val |= WDT_MODE_EN;
		val |= regs->wdt_key_val;
		writel(val, wdt_base + regs->wdt_mode);
	}
	return 0;
}

/*
 * A panic must reset this board even when panic_timeout is 0.  vpanic() only
 * reaches emergency_restart() when panic_timeout != 0, and the TWD is
 * otherwise deliberately disarmed for s2idle (patch 0020, plus the NixOS
 * disarm-before-sleep hook), so a panic on the idle/suspend entry path used to
 * leave the board hung with nobody left to feed the watchdog.  Arm the TWD to
 * its shortest interval and reload it; the hardware then resets the SoC a
 * fraction of a second later.  This runs from an atomic notifier in panic
 * context: only readl()/writel() and printk(), no locks and no sleeping.  It
 * models sunxi_wdt_restart() but returns instead of spinning, so it never
 * blocks the panic path.
 */
static int sunxi_wdt_panic_notify(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	struct sunxi_wdt_dev *sunxi_wdt =
		container_of(nb, struct sunxi_wdt_dev, panic_nb);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;
	u32 val;

	/*
	 * With panic_timeout > 0 vpanic() waits and then calls
	 * emergency_restart() itself; arming the TWD here would reboot out from
	 * under that path (and out from under any restart handler it runs), so
	 * leave it alone.
	 */
	if (panic_timeout > 0)
		return NOTIFY_DONE;

	/* Route a TWD expiry to a full system reset, as sunxi_wdt_restart(). */
	val = readl(wdt_base + regs->wdt_cfg);
	val &= ~regs->wdt_reset_mask;
	val |= regs->wdt_reset_val;
	val |= regs->wdt_key_val;
	writel(val, wdt_base + regs->wdt_cfg);

	/* Shortest hardware interval (0.5 s), enabled, counter reloaded now. */
	val = readl(wdt_base + regs->wdt_mode);
	val &= ~(WDT_TIMEOUT_MASK << regs->wdt_timeout_shift);
	val |= WDT_MODE_EN;
	val |= regs->wdt_key_val;
	writel(val, wdt_base + regs->wdt_mode);
	writel(WDT_CTRL_RELOAD, wdt_base + regs->wdt_ctrl);

	pr_emerg(DRV_NAME ": panic, forcing a reset via the TWD\n");

	return NOTIFY_DONE;
}

static void sunxi_wdt_unregister_panic_notifier(void *data)
{
	struct sunxi_wdt_dev *sunxi_wdt = data;

	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &sunxi_wdt->panic_nb);
}

static int sunxi_wdt_register_panic_notifier(struct device *dev,
					     struct sunxi_wdt_dev *sunxi_wdt)
{
	int err;

	sunxi_wdt->panic_nb.notifier_call = sunxi_wdt_panic_notify;
	err = atomic_notifier_chain_register(&panic_notifier_list,
					     &sunxi_wdt->panic_nb);
	if (err)
		return err;

	/* devm_add_action_or_reset() runs this on both unbind and probe fail. */
	err = devm_add_action_or_reset(dev, sunxi_wdt_unregister_panic_notifier,
				       sunxi_wdt);
	if (err)
		return err;

	dev_info(dev, "panic notifier registered (panic_timeout=%d)\n",
		 panic_timeout);

	return 0;
}

static int sunxi_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;

	writel(WDT_CTRL_RELOAD, wdt_base + regs->wdt_ctrl);

	return 0;
}

static bool sunxi_wdt_is_running(struct watchdog_device *wdt_dev)
{
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;

	return readl(sunxi_wdt->wdt_base + regs->wdt_mode) & WDT_MODE_EN;
}

static unsigned int sunxi_wdt_get_timeout(struct watchdog_device *wdt_dev)
{
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;
	unsigned int timeout;
	u32 interval;

	interval = readl(sunxi_wdt->wdt_base + regs->wdt_mode);
	interval >>= regs->wdt_timeout_shift;
	interval &= WDT_TIMEOUT_MASK;
	/* Round the 0.5-second interval up to the minimum representable timeout. */
	if (!interval)
		return WDT_MIN_TIMEOUT;

	for (timeout = WDT_MIN_TIMEOUT;
	     timeout < ARRAY_SIZE(wdt_timeout_map); timeout++) {
		if (wdt_timeout_map[timeout] == interval)
			return timeout;
	}

	/* Reserved interval encoding. */
	return 0;
}

static int sunxi_wdt_set_timeout(struct watchdog_device *wdt_dev,
		unsigned int timeout)
{
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;
	u32 reg;

	if (wdt_timeout_map[timeout] == 0)
		timeout++;

	sunxi_wdt->wdt_dev.timeout = timeout;

	reg = readl(wdt_base + regs->wdt_mode);
	reg &= ~(WDT_TIMEOUT_MASK << regs->wdt_timeout_shift);
	reg |= wdt_timeout_map[timeout] << regs->wdt_timeout_shift;
	reg |= regs->wdt_key_val;
	writel(reg, wdt_base + regs->wdt_mode);

	sunxi_wdt_ping(wdt_dev);

	return 0;
}

static int sunxi_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;

	writel(regs->wdt_key_val, wdt_base + regs->wdt_mode);

	return 0;
}

static int sunxi_wdt_start(struct watchdog_device *wdt_dev)
{
	u32 reg;
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;
	int ret;

	ret = sunxi_wdt_set_timeout(&sunxi_wdt->wdt_dev,
			sunxi_wdt->wdt_dev.timeout);
	if (ret < 0)
		return ret;

	/* Set system reset function */
	reg = readl(wdt_base + regs->wdt_cfg);
	reg &= ~(regs->wdt_reset_mask);
	reg |= regs->wdt_reset_val;
	reg |= regs->wdt_key_val;
	writel(reg, wdt_base + regs->wdt_cfg);

	/* Enable watchdog */
	reg = readl(wdt_base + regs->wdt_mode);
	reg |= WDT_MODE_EN;
	reg |= regs->wdt_key_val;
	writel(reg, wdt_base + regs->wdt_mode);

	return 0;
}

static const struct watchdog_info sunxi_wdt_info = {
	.identity	= DRV_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops sunxi_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= sunxi_wdt_start,
	.stop		= sunxi_wdt_stop,
	.ping		= sunxi_wdt_ping,
	.set_timeout	= sunxi_wdt_set_timeout,
	.restart	= sunxi_wdt_restart,
};

static const struct sunxi_wdt_reg sun4i_wdt_reg = {
	.wdt_ctrl = 0x00,
	.wdt_cfg = 0x04,
	.wdt_mode = 0x04,
	.wdt_timeout_shift = 3,
	.wdt_reset_mask = 0x02,
	.wdt_reset_val = 0x02,
};

static const struct sunxi_wdt_reg sun6i_wdt_reg = {
	.wdt_ctrl = 0x10,
	.wdt_cfg = 0x14,
	.wdt_mode = 0x18,
	.wdt_timeout_shift = 4,
	.wdt_reset_mask = 0x03,
	.wdt_reset_val = 0x01,
};

static const struct sunxi_wdt_reg sun20i_wdt_reg = {
	.wdt_ctrl = 0x10,
	.wdt_cfg = 0x14,
	.wdt_mode = 0x18,
	.wdt_timeout_shift = 4,
	.wdt_reset_mask = 0x03,
	.wdt_reset_val = 0x01,
	.wdt_key_val = 0x16aa0000,
};

static const struct sunxi_wdt_reg sun55i_wdt_reg = {
	.wdt_ctrl = 0x0c,
	.wdt_cfg = 0x10,
	.wdt_mode = 0x14,
	.wdt_timeout_shift = 4,
	.wdt_reset_mask = 0x03,
	.wdt_reset_val = 0x01,
	.wdt_key_val = 0x16aa0000,
};

static const struct of_device_id sunxi_wdt_dt_ids[] = {
	{ .compatible = "allwinner,sun4i-a10-wdt", .data = &sun4i_wdt_reg },
	{ .compatible = "allwinner,sun6i-a31-wdt", .data = &sun6i_wdt_reg },
	{ .compatible = "allwinner,sun20i-d1-wdt", .data = &sun20i_wdt_reg },
	{ .compatible = "allwinner,sun55i-a523-wdt", .data = &sun55i_wdt_reg },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sunxi_wdt_dt_ids);

static int sunxi_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sunxi_wdt_dev *sunxi_wdt;
	unsigned int running_timeout;
	int err;

	sunxi_wdt = devm_kzalloc_obj(dev, *sunxi_wdt);
	if (!sunxi_wdt)
		return -ENOMEM;

	sunxi_wdt->wdt_regs = of_device_get_match_data(dev);
	if (!sunxi_wdt->wdt_regs)
		return -ENODEV;

	sunxi_wdt->wdt_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sunxi_wdt->wdt_base))
		return PTR_ERR(sunxi_wdt->wdt_base);

	sunxi_wdt->wdt_dev.info = &sunxi_wdt_info;
	sunxi_wdt->wdt_dev.ops = &sunxi_wdt_ops;
	sunxi_wdt->wdt_dev.timeout = WDT_MAX_TIMEOUT;
	sunxi_wdt->wdt_dev.max_timeout = WDT_MAX_TIMEOUT;
	sunxi_wdt->wdt_dev.min_timeout = WDT_MIN_TIMEOUT;
	sunxi_wdt->wdt_dev.parent = dev;

	watchdog_init_timeout(&sunxi_wdt->wdt_dev, timeout, dev);
	watchdog_set_nowayout(&sunxi_wdt->wdt_dev, nowayout);
	watchdog_set_restart_priority(&sunxi_wdt->wdt_dev, 128);

	watchdog_set_drvdata(&sunxi_wdt->wdt_dev, sunxi_wdt);
	platform_set_drvdata(pdev, sunxi_wdt);

	/*
	 * If the bootloader already armed the watchdog, adopt it instead of
	 * disarming it.
	 */
	if (sunxi_wdt_is_running(&sunxi_wdt->wdt_dev)) {
		running_timeout = sunxi_wdt_get_timeout(&sunxi_wdt->wdt_dev);
		sunxi_wdt_set_timeout(&sunxi_wdt->wdt_dev, WDT_MAX_TIMEOUT);
		sunxi_wdt->wdt_dev.max_hw_heartbeat_ms = WDT_MAX_TIMEOUT * 1000;
		set_bit(WDOG_HW_RUNNING, &sunxi_wdt->wdt_dev.status);
		dev_info(dev,
			 "adopted boot watchdog (boot_timeout=%us timeout=%us hw_running=%d)\n",
			 running_timeout, sunxi_wdt->wdt_dev.timeout,
			 test_bit(WDOG_HW_RUNNING, &sunxi_wdt->wdt_dev.status));
	} else {
		sunxi_wdt_stop(&sunxi_wdt->wdt_dev);
	}

	watchdog_stop_on_reboot(&sunxi_wdt->wdt_dev);
	err = devm_watchdog_register_device(dev, &sunxi_wdt->wdt_dev);
	if (unlikely(err))
		return err;

	err = sunxi_wdt_register_panic_notifier(dev, sunxi_wdt);
	if (unlikely(err))
		return err;

	dev_info(dev, "Watchdog enabled (timeout=%d sec, nowayout=%d)",
		 sunxi_wdt->wdt_dev.timeout, nowayout);

	return 0;
}

/*
 * The TWD keeps counting while the system is suspended (its maximum is 16 s
 * on the H616/H700), but nobody can feed it once userspace is frozen and the
 * watchdog core cancels its ping worker.  Disarm it across system sleep so a
 * suspend longer than the timeout cannot reset the board, and re-arm it on
 * resume.  This covers every suspend path, including a raw write to
 * /sys/power/state (e.g. rtcwake), which never runs systemd's sleep hooks.
 */
static int __maybe_unused sunxi_wdt_suspend(struct device *dev)
{
	struct sunxi_wdt_dev *sunxi_wdt = dev_get_drvdata(dev);
	struct watchdog_device *wdd = &sunxi_wdt->wdt_dev;

	if (watchdog_active(wdd) || watchdog_hw_running(wdd))
		sunxi_wdt_stop(wdd);

	return 0;
}

static int __maybe_unused sunxi_wdt_resume(struct device *dev)
{
	struct sunxi_wdt_dev *sunxi_wdt = dev_get_drvdata(dev);
	struct watchdog_device *wdd = &sunxi_wdt->wdt_dev;

	if (watchdog_active(wdd) || watchdog_hw_running(wdd))
		sunxi_wdt_start(wdd);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(sunxi_wdt_pm_ops, sunxi_wdt_suspend,
				sunxi_wdt_resume);

static struct platform_driver sunxi_wdt_driver = {
	.probe		= sunxi_wdt_probe,
	.driver		= {
		.name		= DRV_NAME,
		.of_match_table	= sunxi_wdt_dt_ids,
		.pm		= pm_ptr(&sunxi_wdt_pm_ops),
	},
};

module_platform_driver(sunxi_wdt_driver);

module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout, "Watchdog heartbeat in seconds");

module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started "
		"(default=" __stringify(WATCHDOG_NOWAYOUT) ")");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Carlo Caione <carlo.caione@gmail.com>");
MODULE_AUTHOR("Henrik Nordstrom <henrik@henriknordstrom.net>");
MODULE_DESCRIPTION("sunxi WatchDog Timer Driver");
MODULE_VERSION(DRV_VERSION);
