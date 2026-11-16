// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Daniel Palmer
 */

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-msc313-bindings.h"

#include "clk-msc313-mux.h"


static const struct clk_parent_data mcu_pm_parents[] = {
	{ .fw_name = "mpll_div_4" },
};

static const struct clk_parent_data spi_pm_parents[] = {
	{ .fw_name = "rtc_xtal" },
};

#define MCU_PM MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_MCU_PM, "mcu_pm", mcu_pm_parents, 0x80, 0, 2, 4, 7)
#define SPI_PM MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_SPI_PM, "spi_pm", spi_pm_parents, 0x80, 8, 10, 4, 14)

static const struct clk_parent_data ir_parents[] = {
	{ .fw_name = "xtal_div2" },
	{ .fw_name = "rtc_xtal" },
	{ .fw_name = "xtal_div2_div8" },
	{ .fw_name = "xtal_div2_div12" },
	{ .fw_name = "rtc_xtal_div4" },
	{ .fw_name = "xtal_div2_div16" },
	{ .fw_name = "xtal_div2_div2" },
	{ .fw_name = "xtal_div2_div4" },
};

#define IR MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_IR, "ir", ir_parents, 0x84, 5, 7, 3, -1)

static const struct clk_parent_data rtc_parents[] = {
	{ .fw_name = "xtal_div2" },
	{ .fw_name = "rtc_xtal" },
};

static const struct clk_parent_data sar_pm_sleep_parents[] = {
	{ .fw_name = "xtal_div2" },
	{ .fw_name = "rtc_xtal" },
	{ .fw_name = "xtal_div2_div8" },
	{ .fw_name = "xtal_div2_div12" },
	{ .fw_name = "rtc_xtal_div4" },
	{ .fw_name = "xtal_div2_div16" },
	{ .fw_name = "xtal_div2_div2" },
	{ .fw_name = "xtal_div2_div4" },
};

#define RTC		MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_RTC, "rtc", rtc_parents, 0x88, 0, 2, 2, -1)
#define SAR		MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_SAR, "sar", sar_pm_sleep_parents, 0x88, 5, 7, 3, -1)
#define PM_SLEEP	MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_PM_SLEEP, "pm_sleep", sar_pm_sleep_parents, 0x88, 10, 12, 3, -1)

static const struct msc313_mux_data msc313_muxes[] = {
	MCU_PM,
	SPI_PM,
	IR,
	RTC,
	SAR,
	PM_SLEEP,
};

static const struct msc313_muxes_data msc313_data = MSC313_MUXES_DATA(msc313_muxes);

static const struct msc313_mux_data ssd20xd_muxes[] = {
	MCU_PM,
	SPI_PM,
	IR,
	RTC,
	SAR,
	PM_SLEEP,
};

static const struct msc313_muxes_data ssd20xd_data = MSC313_MUXES_DATA(ssd20xd_muxes);

static const struct of_device_id msc313_pm_muxes_ids[] = {
	{
		.compatible = "mstar,msc313-pm-muxes",
		.data = &msc313_data,
	},
	{
		.compatible = "sstar,ssd20xd-pm-muxes",
		.data = &ssd20xd_data,
	},
	{}
};

static int msc313_pm_muxes_probe(struct platform_device *pdev)
{
	const struct msc313_muxes_data *muxes_data;
	struct device *dev = &pdev->dev;
	struct msc313_muxes *muxes;
	struct regmap *regmap;

	muxes_data = of_device_get_match_data(dev);
	if (!muxes_data)
		return -EINVAL;

	if (!dev->parent)
		return -ENODEV;

	regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	muxes = msc313_mux_register_muxes(dev, regmap, muxes_data, NULL, NULL);
	if (IS_ERR(muxes))
		return PTR_ERR(muxes);

	return devm_of_clk_add_hw_provider(dev, msc313_mux_xlate, muxes);
}

static struct platform_driver msc313_pm_muxes_driver = {
	.driver = {
		.name = "msc313-pm-muxes",
		.of_match_table = msc313_pm_muxes_ids,
	},
	.probe = msc313_pm_muxes_probe,
};
builtin_platform_driver(msc313_pm_muxes_driver);
