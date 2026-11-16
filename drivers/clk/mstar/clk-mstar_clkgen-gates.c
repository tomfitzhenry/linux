// SPDX-License-Identifier: GPL-2.0
/*
 * MStar clkgen gates driver
 *
 * Ported to the 7.4 CCF API from
 * linux-chenxing/mstar_v6_7_rebase:drivers/clk/mstar/clk-mstar_clkgen-gates.c
 *
 * Copyright (C) 2020 Daniel Palmer
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

struct mstar_clkgen_gates {
	spinlock_t lock;
};

static const struct of_device_id mstar_clkgen_gates_of_match[] = {
	{ .compatible = "mstar,clkgen-gates" },
	{}
};

static int mstar_clkgen_gates_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mstar_clkgen_gates *gates;
	struct clk_hw_onecell_data *clk_data;
	const char *parents[32];
	const char *name;
	void __iomem *base;
	u32 gateshift, outputflags;
	int numparents, numgates, i, ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	numparents = of_clk_parent_fill(dev->of_node, parents, ARRAY_SIZE(parents));
	if (numparents <= 0)
		return dev_err_probe(dev, numparents ? numparents : -EINVAL,
				     "failed to get clock parents\n");

	if (numparents != of_clk_get_parent_count(dev->of_node))
		return -EPROBE_DEFER;

	numgates = of_property_count_strings(dev->of_node, "clock-output-names");
	if (numgates <= 0)
		return dev_err_probe(dev, -ENODEV,
				     "clock-output-names must be specified\n");

	if (numparents != numgates)
		return dev_err_probe(dev, -EINVAL,
				     "number of outputs must match number of parents\n");

	if (of_property_count_u32_elems(dev->of_node, "shifts") != numgates)
		return dev_err_probe(dev, -EINVAL,
				     "number of shifts must match number of outputs\n");

	gates = devm_kzalloc_obj(dev, *gates);
	if (!gates)
		return -ENOMEM;

	spin_lock_init(&gates->lock);

	clk_data = devm_kzalloc_flex(dev, *clk_data, hws, numgates);
	if (!clk_data)
		return -ENOMEM;

	clk_data->num = numgates;

	for (i = 0; i < numgates; i++) {
		ret = of_property_read_string_index(dev->of_node,
						    "clock-output-names", i, &name);
		if (ret)
			return ret;

		ret = of_property_read_u32_index(dev->of_node, "shifts", i,
						 &gateshift);
		if (ret)
			return ret;

		outputflags = 0;
		of_property_read_u32_index(dev->of_node, "output-flags", i,
					   &outputflags);

		clk_data->hws[i] = devm_clk_hw_register_gate(dev, name,
				parents[i], outputflags, base, gateshift,
				CLK_GATE_SET_TO_DISABLE, &gates->lock);
		if (IS_ERR(clk_data->hws[i]))
			return dev_err_probe(dev, PTR_ERR(clk_data->hws[i]),
					     "failed to register gate %s\n", name);
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
}

static struct platform_driver mstar_clkgen_gates_driver = {
	.driver = {
		.name = "mstar-clkgen-gates",
		.of_match_table = mstar_clkgen_gates_of_match,
	},
	.probe = mstar_clkgen_gates_probe,
};
builtin_platform_driver(mstar_clkgen_gates_driver);
