/*
 * drivers/watchdog/tegra_wdt.c
 *
 * watchdog driver for NVIDIA tegra internal watchdog
 *
 * Copyright (c) 2012-2015, NVIDIA CORPORATION. All rights reserved.
 *
 * based on drivers/watchdog/softdog.c and drivers/watchdog/omap_wdt.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

/* minimum and maximum watchdog trigger timeout, in seconds */
#define MIN_WDT_TIMEOUT			5
#define MAX_WDT_TIMEOUT			1000
/* Assign Timer 7 to Timer 10 for WDT0 to WDT3, respectively */
#define TMR_SRC_START	7

/* WDT registers */
#define WDT_CFG				0x0
 #define WDT_CFG_PERIOD			(1 << 4)
 #define WDT_CFG_INT_EN			(1 << 12)
 #define WDT_CFG_FIQ_INT_EN		(1 << 13)
 #define WDT_CFG_SYS_RST_EN		(1 << 14)
 #define WDT_CFG_PMC2CAR_RST_EN		(1 << 15)
#define WDT_STS				0x4
 #define WDT_INTR_STAT			(1 << 1)
#define WDT_CMD				0x8
#define WDT_CMD_START_COUNTER		(1 << 0)
#define WDT_CMD_DISABLE_COUNTER		(1 << 1)
#define WDT_UNLOCK			(0xc)
#define WDT_UNLOCK_PATTERN		(0xc45a << 0)
#define ICTLR_IEP_CLASS			0x2C
#define MAX_NR_CPU_WDT			0x4
#define PMC_RST_STATUS			0x1b4


/* Timer registers */
#define TIMER_PTV			0x0
#define TIMER_EN			(1 << 31)
#define TIMER_PERIODIC			(1 << 30)
#define TIMER_PCR			0x4
 #define TIMER_PCR_INTR			(1 << 30)

struct tegra_wdt {
	struct watchdog_device	wdd;
	struct resource		*res_src;
	struct resource		*res_wdt;
	void __iomem		*wdt_regs;
	void __iomem		*tmr_regs;
	int			tmrsrc;
};

struct tegra_wdt *tegra_wdt[MAX_NR_CPU_WDT];

/*
 * For spinlock lockup detection to work, the heartbeat should be 2*lockup
 * for cases where the spinlock disabled irqs.
 */
#define WDT_HEARTBEAT 80
static int heartbeat = WDT_HEARTBEAT;
module_param(heartbeat, int, 0);
MODULE_PARM_DESC(heartbeat,
	"Watchdog heartbeats in seconds. (default = "
	__MODULE_STRING(WDT_HEARTBEAT) ")");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
	"Watchdog cannot be stopped once started (default="
	__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");
static int tegra_wdt_start(struct watchdog_device *wdd)
{
	struct tegra_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 val;

	writel(TIMER_PCR_INTR, wdt->tmr_regs + TIMER_PCR);
	val = (wdd->timeout * 1000000ul) / 4;
	val |= (TIMER_EN | TIMER_PERIODIC);
	writel(val, wdt->tmr_regs + TIMER_PTV);

	/* Interrupt handler is not required for user space
	 * WDT accesses, since the caller is responsible to ping the
	 * WDT to reset the counter before expiration, through ioctls.
	 * SYS_RST_EN doesnt work as there is no external reset
	 * from Tegra.
	 */
	val = wdt->tmrsrc | WDT_CFG_PERIOD | /*WDT_CFG_INT_EN |*/
		/*WDT_CFG_SYS_RST_EN |*/ WDT_CFG_PMC2CAR_RST_EN;
	writel(val, wdt->wdt_regs + WDT_CFG);

	writel(WDT_CMD_START_COUNTER, wdt->wdt_regs + WDT_CMD);

	return 0;
}

static int tegra_wdt_stop(struct watchdog_device *wdd)
{
	struct tegra_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(WDT_UNLOCK_PATTERN, wdt->wdt_regs + WDT_UNLOCK);
	writel(WDT_CMD_DISABLE_COUNTER, wdt->wdt_regs + WDT_CMD);
	writel(0, wdt->tmr_regs + TIMER_PTV);

	return 0;
}

static int tegra_wdt_ping(struct watchdog_device *wdd)
{
	struct tegra_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(WDT_CMD_START_COUNTER, wdt->wdt_regs + WDT_CMD);

	return 0;
}

static int tegra_wdt_set_timeout(struct watchdog_device *wdd,
				 unsigned int timeout)
{
	wdd->timeout = timeout;

	if (watchdog_active(wdd)) {
		tegra_wdt_stop(wdd);
		return tegra_wdt_start(wdd);
	}

	return 0;
}
 
static const struct watchdog_info tegra_wdt_info = {
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_MAGICCLOSE |
			  WDIOF_KEEPALIVEPING,
	.firmware_version = 0,
	.identity	= "Tegra Watchdog",
};

static const struct watchdog_ops tegra_wdt_ops = {
	.owner = THIS_MODULE,
	.start = tegra_wdt_start,
	.stop = tegra_wdt_stop,
	.ping = tegra_wdt_ping,
	.set_timeout = tegra_wdt_set_timeout,
};

static int tegra_wdt_probe(struct platform_device *pdev)
{
	struct watchdog_device *wdd;
	struct tegra_wdt *wdt;
	struct resource *res_src, *res_wdt, *res_irq;
	int ret = 0;

	if ((pdev->id < -1) || (pdev->id > 0)) {
		dev_err(&pdev->dev, "Only support IDs -1 and 0\n");
		return -ENODEV;
	}

	/* This is the timer base. */
	res_src = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	res_wdt = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	res_irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);

	if (!res_src || !res_wdt || (!pdev->id && !res_irq)) {
		dev_err(&pdev->dev, "incorrect resources\n");
		return -ENOENT;
	}

	if (pdev->id == -1 && !res_irq) {
		dev_err(&pdev->dev, "incorrect irq\n");
		return -ENOENT;
	}

	/*
	 * Allocate our watchdog driver data, which has the
	 * struct watchdog_device nested within it.
	 */
	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	/* Initialize struct tegra_wdt. */
	wdt->wdt_regs = devm_ioremap_resource(&pdev->dev, res_src);
	wdt->tmr_regs = devm_ioremap_resource(&pdev->dev, res_wdt);

	/* tmrsrc will be used to set WDT_CFG */
	wdt->tmrsrc = (TMR_SRC_START + pdev->id) % 10;

	if (!wdt->wdt_regs || !wdt->tmr_regs) {
		dev_err(&pdev->dev, "unable to map registers\n");
		ret = -ENOMEM;
		goto fail;
	}

	/* Initialize struct watchdog_device. */
	wdd = &wdt->wdd;
	wdd->timeout = heartbeat;
	wdd->info = &tegra_wdt_info;
	wdd->ops = &tegra_wdt_ops;
	wdd->min_timeout = MIN_WDT_TIMEOUT;
	wdd->max_timeout = MAX_WDT_TIMEOUT;
	wdd->parent = &pdev->dev;

	watchdog_set_drvdata(wdd, wdt);

	watchdog_set_nowayout(wdd, nowayout);

	ret = watchdog_register_device(wdd);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to register watchdog device\n");
		return ret;
	}

	platform_set_drvdata(pdev, wdt);

	tegra_wdt_stop(&wdt->wdd);
	writel(TIMER_PCR_INTR, wdt->tmr_regs + TIMER_PCR);

	wdt->res_src = res_src;
	wdt->res_wdt = res_wdt;

	tegra_wdt[pdev->id] = wdt;

	dev_info(&pdev->dev,
		 "initialized (heartbeat = %d sec, nowayout = %d)\n",
		 heartbeat, nowayout);
	return 0;
fail:
	return ret;
}

static int tegra_wdt_remove(struct platform_device *pdev)
{
	struct tegra_wdt *wdt = platform_get_drvdata(pdev);

	tegra_wdt_stop(&wdt->wdd);

	platform_set_drvdata(pdev, NULL);

	watchdog_unregister_device(&wdt->wdd);

	dev_info(&pdev->dev, "removed wdt\n");
	return 0;
}

#ifdef CONFIG_PM
static int tegra_wdt_runtime_suspend(struct device *dev)
{
	struct tegra_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_active(&wdt->wdd))
		tegra_wdt_stop(&wdt->wdd);

	return 0;
}

static int tegra_wdt_runtime_resume(struct device *dev)
{
	struct tegra_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_active(&wdt->wdd))
		tegra_wdt_start(&wdt->wdd);

	return 0;
}

static const struct dev_pm_ops tegra_wdt_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(tegra_wdt_runtime_suspend,
				tegra_wdt_runtime_resume)
};
#endif

static struct platform_driver tegra_wdt_driver = {
	.probe		= tegra_wdt_probe,
	.remove		= tegra_wdt_remove,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "tegra_wdt",
		.pm	= &tegra_wdt_pm_ops,
	},
};

module_platform_driver(tegra_wdt_driver);

MODULE_AUTHOR("NVIDIA Corporation");
MODULE_DESCRIPTION("Tegra Watchdog Driver");

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:tegra_wdt");
