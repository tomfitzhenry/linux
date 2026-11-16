// SPDX-License-Identifier: GPL-2.0
/*
 * MStar MSC313 UPLL driver
 *
 * Ported to the 7.4 CCF API from
 * linux-chenxing/mstar_v6_7_rebase:drivers/clk/mstar/clk-msc313-upll.c
 *
 * Copyright (C) 2019 Daniel Palmer
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/*
 * 0x0  - ??
 * write 0x00c0 - enable
 * write 0x01b2 - disable
 *
 * 0x1c - ??
 *         1         |        0
 * set when disabled | set when enabled
 */

#define REG_MAGIC	0x0
#define REG_ENABLED	0x1c
#define REG_EN_PWRDN	0x1c

/* Based on the upll powerdown code in the i2m sdk kernel */
static const struct reg_field pd_field = REG_FIELD(REG_MAGIC, 1, 1);
static const struct reg_field ctrl_pd_clk0_audio_field = REG_FIELD(REG_EN_PWRDN, 3, 3);

static const char *const msc313_upll_outputs[] = {
	"384",
	"320",
};

static const u32 msc313_upll_output_rates[] = {
	384000000,
	320000000,
};

struct mstar_upll;

struct mstar_upll_output {
	struct mstar_upll *pll;
	struct clk_hw clk_hw;
	u32 rate;
};

struct mstar_upll {
	void __iomem *base;
	struct regmap_field *pd;
	struct regmap_field *pd_clk0_audio;
	struct mstar_upll_output outputs[];
};

#define to_pll_output(_hw) container_of(_hw, struct mstar_upll_output, clk_hw)

static int msc313_upll_is_enabled(struct clk_hw *hw)
{
	struct mstar_upll_output *output = to_pll_output(hw);

	return ioread16(output->pll->base + REG_ENABLED) & BIT(0);
}

static unsigned long msc313_upll_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct mstar_upll_output *output = to_pll_output(hw);

	return output->rate;
}

static const struct clk_ops msc313_upll_ops = {
	.is_enabled = msc313_upll_is_enabled,
	.recalc_rate = msc313_upll_recalc_rate,
};

static const struct of_device_id msc313_upll_of_match[] = {
	{ .compatible = "mstar,msc313-upll" },
	{}
};

static const struct regmap_config msc313_upll_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static int msc313_upll_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *clk_data;
	struct clk_init_data clk_init = {};
	const char *parents[1];
	struct mstar_upll *upll;
	struct regmap *regmap;
	int numoutputs, numparents, i, ret;

	numoutputs = ARRAY_SIZE(msc313_upll_outputs);

	upll = devm_kzalloc_flex(dev, *upll, outputs, numoutputs);
	if (!upll)
		return -ENOMEM;

	upll->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(upll->base))
		return PTR_ERR(upll->base);

	numparents = of_clk_parent_fill(dev->of_node, parents, ARRAY_SIZE(parents));
	if (numparents != 1)
		return dev_err_probe(dev, -EINVAL, "expected exactly one parent clock\n");

	clk_data = devm_kzalloc_flex(dev, *clk_data, hws, numoutputs);
	if (!clk_data)
		return -ENOMEM;
	clk_data->num = numoutputs;

	for (i = 0; i < numoutputs; i++) {
		struct mstar_upll_output *output = &upll->outputs[i];

		output->pll = upll;
		output->rate = msc313_upll_output_rates[i];

		clk_init.name = devm_kasprintf(dev, GFP_KERNEL, "%s_%s",
				dev_name(dev), msc313_upll_outputs[i]);
		clk_init.ops = &msc313_upll_ops;
		clk_init.num_parents = 1;
		clk_init.parent_names = parents;
		output->clk_hw.init = &clk_init;

		ret = devm_clk_hw_register(dev, &output->clk_hw);
		if (ret)
			return ret;

		clk_data->hws[i] = &output->clk_hw;
	}

	regmap = devm_regmap_init_mmio(dev, upll->base, &msc313_upll_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	iowrite16(0x00c0, upll->base + REG_MAGIC);
	iowrite8(0x01, upll->base + REG_ENABLED);

	upll->pd = devm_regmap_field_alloc(dev, regmap, pd_field);
	if (IS_ERR(upll->pd))
		return PTR_ERR(upll->pd);

	upll->pd_clk0_audio = devm_regmap_field_alloc(dev, regmap,
						      ctrl_pd_clk0_audio_field);
	if (IS_ERR(upll->pd_clk0_audio))
		return PTR_ERR(upll->pd_clk0_audio);

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
}

static struct platform_driver msc313_upll_driver = {
	.driver = {
		.name = "msc313-upll",
		.of_match_table = msc313_upll_of_match,
	},
	.probe = msc313_upll_probe,
};
builtin_platform_driver(msc313_upll_driver);
