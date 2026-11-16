// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Daniel Palmer
 */

#include <linux/clk-provider.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include "clk-msc313-bindings.h"

#include "clk-msc313-mux.h"


#define REG_GATES	0x1c0
#define REG_LOCK	(REG_GATES + 0x0)
#define REG_LOCK_OFF	BIT(1)
#define REG_FORCEON	(REG_GATES + 0x4)
#define REG_FORCEOFF	(REG_GATES + 0x8)
#define REG_ENRD	(REG_GATES + 0xc)

struct msc313_clkgen_parent_data {
	const char *fw_name;
	int mux_idx;
	int gate;
	int divider;
};

#define GATE_NODIVIDER	-1

struct msc313_clkgen_gate_data {
	const struct clk_parent_data clk_parent_data;
	const unsigned int *dividers;
	const unsigned int num_dividers;
};

#define GATE_DATA(_parent, _dividers) \
	{							\
		.clk_parent_data = { .fw_name = _parent },	\
		.dividers = _dividers,				\
		.num_dividers = ARRAY_SIZE(_dividers),		\
	}

#define GATE_DATA_NO_DIVIDERS(_parent) \
	{							\
		.clk_parent_data = { .fw_name = _parent },	\
	}

static const unsigned int gate2_dividers[] = { 4, 5, 8 };

#define GATE2_DIVIDEBY_4 0
#define GATE2_DIVIDEBY_5 1
#define GATE2_DIVIDEBY_8 2

static const unsigned int gate3_dividers[] = { 4 };

#define GATE3_DIVIDEBY_4 0

static const unsigned int gate8_dividers[] = { 2, 4, 8 };

#define GATE8_DIVIDEBY_2 0
#define GATE8_DIVIDEBY_4 1
#define GATE8_DIVIDEBY_8 2

static const unsigned int gate9_dividers[] = { 2, 4, 8 };

#define GATE9_DIVIDEBY_2 0
#define GATE9_DIVIDEBY_4 1
#define GATE9_DIVIDEBY_8 2

static const unsigned int gate11_dividers[] = { 2, 4 };
static const unsigned int gate12_dividers[] = { 2 };

static const unsigned int gate14_dividers[] = { 2, 4, 16 };

#define GATE14_DIVIDEBY_2 0
#define GATE14_DIVIDEBY_4 1
#define GATE14_DIVIDEBY_16 2

static const struct msc313_clkgen_gate_data gates_data[] = {
	/* upll 384 */
	GATE_DATA_NO_DIVIDERS("gate0"),
	/* upll 320 */
	GATE_DATA_NO_DIVIDERS("gate1"),
	/* utmi 160 */
	GATE_DATA("gate2", gate2_dividers),
	/* utmi 192 */
	GATE_DATA("gate3", gate3_dividers),
	/* utmi 240 */
	GATE_DATA_NO_DIVIDERS("gate4"),
	GATE_DATA_NO_DIVIDERS("gate5"),
	/* mpll 432 */
	GATE_DATA_NO_DIVIDERS("gate6"),
	/* mpll 172 */
	GATE_DATA_NO_DIVIDERS("gate7"),
	/* mpll 288 */
	GATE_DATA("gate8", gate8_dividers),
	/* mpll 216 */
	GATE_DATA("gate9", gate9_dividers),
	/* mpll 172 */
	GATE_DATA_NO_DIVIDERS("gate10"),
	GATE_DATA("gate11", gate11_dividers),
	/* 123 */
	GATE_DATA("gate12", gate12_dividers),
	/* 124 */
	GATE_DATA_NO_DIVIDERS("gate13"),
	/* 86 */
	GATE_DATA("gate14", gate14_dividers),
	GATE_DATA_NO_DIVIDERS("gate15"),
};

struct msc313_clkgen_gate {
	struct regmap_field *force_on;
	struct regmap_field *enrd;
	struct clk_hw clk_hw;
	struct clk_hw **clk_hw_dividers;
};
#define to_gate(_hw) container_of(_hw, struct msc313_clkgen_gate, clk_hw)

struct msc313_clkgen {
	struct device *dev;
	struct msc313_muxes *muxes;
	struct msc313_clkgen_gate gates[];
};

static int msc313_clkgen_gate_enable(struct clk_hw *hw)
{
	struct msc313_clkgen_gate *gate = to_gate(hw);

	return regmap_field_write(gate->force_on, 1);
}

static void msc313_clkgen_gate_disable(struct clk_hw *hw)
{
	struct msc313_clkgen_gate *gate = to_gate(hw);

	regmap_field_write(gate->force_on, 0);
}

static int msc313_clkgen_gate_is_enabled(struct clk_hw *hw)
{
	struct msc313_clkgen_gate *gate = to_gate(hw);
	unsigned int val;
	int ret;

	ret = regmap_field_read(gate->enrd, &val);
	if (ret)
		return ret;

	return val;
}

static const struct clk_ops msc313_clkgen_gate_ops = {
	.enable = msc313_clkgen_gate_enable,
	.disable = msc313_clkgen_gate_disable,
	.is_enabled = msc313_clkgen_gate_is_enabled,
};

#define PARENT_GATE(_gate)			\
	{					\
		.mux_idx = -1,			\
		.gate = _gate,			\
		.divider = GATE_NODIVIDER,	\
	}

#define PARENT_DIVIDER(_gate, _divisor)				\
	{							\
		.mux_idx = -1,					\
		.gate = _gate,					\
		.divider = GATE ##_gate##_DIVIDEBY_##_divisor,	\
	}

#define PARENT_OF(_fwname)			\
	{					\
		.fw_name = _fwname,		\
	}

#define PARENT_MUX(_mux_idx)			\
	{					\
		.mux_idx = _mux_idx,		\
	}




static const struct msc313_clkgen_parent_data disp_432_parents[] = {
	PARENT_GATE(6),
};
#define DISP_432	MSC313_MUX_PARENT_DATA(SSD20XD_CLKGEN_DISP_432, "disp_432", disp_432_parents, 0x14c, 0, 2, 2, -1)

static const struct msc313_clkgen_parent_data disp_216_parents[] = {
	PARENT_GATE(9),
};
#define DISP_216	MSC313_MUX_PARENT_DATA(SSD20XD_CLKGEN_DISP_216, "disp_216", disp_216_parents, 0x14c, 8, 10, 2, -1)

/* ssd20xd only? */
static const struct msc313_clkgen_parent_data mop_parents[] = {
	PARENT_MUX(MSC313_CLKGEN_MIU), // wrong
	PARENT_MUX(MSC313_CLKGEN_MIU), // wrong
	PARENT_GATE(8),
	PARENT_MUX(MSC313_CLKGEN_MIU),
};
#define MOP	MSC313_MUX_PARENT_DATA(MSC313_CLKGEN_MOP, "mop", mop_parents, 0x150, 0, 2, 2, -1)

/* DEC PCLK */
static const struct msc313_clkgen_parent_data dec_pclk_parents[] = {
	PARENT_GATE(9),
	PARENT_DIVIDER(8, 2),
	PARENT_DIVIDER(9, 2),
};
#define DEC_PCLK	MSC313_MUX_PARENT_DATA(SSD20XD_CLKGEN_DEC_PCLK, "dec_pclk", dec_pclk_parents, 0x154, 0, 2, 2, -1)

/* DEC ACLK */
static const struct msc313_clkgen_parent_data dec_aclk_parents[] = {
	PARENT_GATE(1),
	PARENT_GATE(0),
	PARENT_GATE(8),
	PARENT_GATE(9),
};
#define DEC_ACLK	MSC313_MUX_PARENT_DATA(SSD20XD_CLKGEN_DEC_ACLK, "dec_aclk", dec_aclk_parents, 0x154, 8, 10, 2, -1)

/* DEC BCLK */
static const struct msc313_clkgen_parent_data dec_bclk_parents[] = {
	PARENT_GATE(8),
};
#define DEC_BCLK	MSC313_MUX_PARENT_DATA(SSD20XD_CLKGEN_DEC_BCLK, "dec_bclk", dec_bclk_parents, 0x1f8, 0, 2, 3, -1)

/* DEC CCLK */
static const struct msc313_clkgen_parent_data dec_cclk_parents[] = {
	PARENT_GATE(0),
};
#define DEC_CCLK	MSC313_MUX_PARENT_DATA(SSD20XD_CLKGEN_DEC_CCLK, "dec_cclk", dec_cclk_parents, 0x1f8, 8, 10, 3, -1)

static const struct msc313_clkgen_parent_data sc_pixel_parents[] = {
	PARENT_GATE(9), // WRONG!! should be 240MHz -- utmi 240
	PARENT_GATE(9),
	PARENT_GATE(3),
	PARENT_GATE(7),
	PARENT_GATE(7), // WRONG!! should be 144MHz
	PARENT_GATE(12),
	PARENT_DIVIDER(9, 2),
	PARENT_GATE(14),
	PARENT_GATE(7), // WRONG!! should be 72MHz
	PARENT_DIVIDER(9, 4),
	PARENT_OF("lpll"),
};
#define SC_PIXEL MSC313_MUX_PARENT_DATA(SSD20XD_CLKGEN_SC_PIXEL, "sc_pixel", sc_pixel_parents, 0x18c, 0, 2, 4, -1)

static const struct msc313_clkgen_parent_data jpe_parents[] = {
	PARENT_GATE(8),
	PARENT_GATE(9),
	PARENT_DIVIDER(8, 4),
	PARENT_DIVIDER(9, 4),
};
#define JPE	MSC313_MUX_PARENT_DATA(MSC313_CLKGEN_JPE, "jpe", jpe_parents, 0x1a8, 0, 2, 2, -1)

/* SATA */
static const struct msc313_clkgen_parent_data sata_parents[] = {
	PARENT_GATE(9), // incorrect, should be utmi 240m
	PARENT_GATE(8),
};
#define SATA	MSC313_MUX_PARENT_DATA(SSD20XD_CLKGEN_SATA, "sata", sata_parents, 0x1b8, 0, 2, 2, -1)

/*
 * Each chip's mux table starts with these common muxes. Some of them are
 * slightly different (i.e. a different number of parents) on some chips, in
 * which case the per-chip header provides a variant.
 */
#include "clk-msc313-clkgen-msc313.h"
#include "clk-msc313-clkgen-ssd20xd.h"
#include "clk-msc313-clkgen-ssd210.h"

static const struct of_device_id msc313_clkgen_ids[] = {
	{
		.compatible = "mstar,msc313-clkgen",
		.data = &msc313_data,
	},
	{
		.compatible = "sstar,ssd20xd-clkgen",
		.data = &ssd20xd_data,
	},
	{
		.compatible = "sstar,ssd210-clkgen",
		.data = &ssd210_data,
	},
	{}
};

static const struct regmap_config msc313_clkgen_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static struct msc313_mux *msc313_clkgen_xlate_idx_to_mux(struct msc313_clkgen *clkgen, unsigned int idx)
{
	const int nummuxes = clkgen->muxes->muxes_data->num_muxes;

	/*
	 * Find the array offset for the mux that has the desired index
	 * then use the index to return the allocated mux structure.
	 */
	for (int i = 0; i < nummuxes; i++)
		if (clkgen->muxes->muxes_data->muxes[i].index == idx)
			return clkgen->muxes->muxes + i;

	dev_err(clkgen->dev, "Index %d does not map to a mux\n", idx);

	return NULL;
}

static struct clk_hw *msc313_clkgen_xlate(struct of_phandle_args *clkspec, void *data)
{
	struct msc313_clkgen *clkgen = data;
	unsigned int area = clkspec->args[0];
	unsigned int idx = clkspec->args[1];
	unsigned int divider;
	struct msc313_mux *mux;

	switch (area) {
	case MSC313_CLKGEN_MUXES:
		mux = msc313_clkgen_xlate_idx_to_mux(clkgen, idx);
		if (!mux)
			return ERR_PTR(-EINVAL);
		return &mux->mux_hw;
	case MSC313_CLKGEN_DEGLITCHES:
		mux = msc313_clkgen_xlate_idx_to_mux(clkgen, idx);
		if (!mux)
			return ERR_PTR(-EINVAL);
		return &mux->deglitch_hw;
	case MSC313_CLKGEN_GATES:
		return &clkgen->gates[idx].clk_hw;
	case MSC313_CLKGEN_DIVIDERS:
		divider = clkspec->args[2];
		return clkgen->gates[idx].clk_hw_dividers[divider];
	}

	return msc313_mux_xlate(clkspec, clkgen->muxes);
}

static int msc313_clkgen_fill_mux_clk_parent_data(struct clk_parent_data *clk_parent_data,
				void *data, const void *parent_data,
				const struct msc313_muxes *muxes,
				unsigned int mux_idx, unsigned int parent_idx)
{
	struct msc313_clkgen *clkgen = data;
	const struct msc313_clkgen_parent_data *mux_parent_data = parent_data;
	const struct msc313_clkgen_parent_data *mux_data = &mux_parent_data[parent_idx];

	if (mux_data->fw_name)
		clk_parent_data->fw_name = mux_data->fw_name;
	else if (mux_data->mux_idx != -1) {
		/* We can only refer to muxes that have already been created */
		if (mux_data->mux_idx >= mux_idx)
			return -EINVAL;
		clk_parent_data->hw = &muxes->muxes[mux_data->mux_idx].deglitch_hw;
	} else if (mux_data->divider == -1)
		clk_parent_data->hw = &clkgen->gates[mux_data->gate].clk_hw;
	else
		clk_parent_data->hw = clkgen->gates[mux_data->gate].clk_hw_dividers[mux_data->divider];

	return 0;
}

static int msc313_clkgen_probe(struct platform_device *pdev)
{
	const struct msc313_muxes_data *match_data;
	struct clk_init_data gate_clk_init = {
		.ops = &msc313_clkgen_gate_ops,
		.num_parents = 1,
	};
	struct device *dev = &pdev->dev;
	struct msc313_clkgen *clkgen;
	int gate_index;
	struct regmap *regmap;
	void __iomem *base;
	int ret;

	match_data = of_device_get_match_data(dev);
	if (!match_data)
		return -EINVAL;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &msc313_clkgen_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	clkgen = devm_kzalloc_flex(dev, *clkgen, gates, ARRAY_SIZE(gates_data));
	if (!clkgen)
		return -ENOMEM;

	clkgen->dev = dev;

	/* Clear the force on register so we can actually control the gates */
	regmap_write(regmap, REG_FORCEON, 0x0);
	/* Clear the force off register */
	regmap_write(regmap, REG_FORCEOFF, 0x0);
	/* lock the force off bits */
	regmap_write(regmap, REG_LOCK, REG_LOCK_OFF);

	for (gate_index = 0; gate_index < ARRAY_SIZE(gates_data); gate_index++) {
		const struct msc313_clkgen_gate_data *gate_data = &gates_data[gate_index];
		const struct reg_field forceon_field = REG_FIELD(REG_FORCEON,
				gate_index, gate_index);
		const struct reg_field enrd_field = REG_FIELD(REG_ENRD,
				gate_index, gate_index);
		struct msc313_clkgen_gate *gate = &clkgen->gates[gate_index];
		char *divider_name;
		int divider_index;

		gate->force_on = devm_regmap_field_alloc(dev, regmap, forceon_field);
		gate->enrd = devm_regmap_field_alloc(dev, regmap, enrd_field);
		gate->clk_hw.init = &gate_clk_init;

		gate_clk_init.parent_data = &gates_data[gate_index].clk_parent_data;
		gate_clk_init.name = devm_kasprintf(dev, GFP_KERNEL, "%s_gate_%d",
				dev_name(dev), gate_index);

		ret = devm_clk_hw_register(dev, &gate->clk_hw);
		if (ret)
			return ret;

		if (!gate_data->num_dividers)
			goto no_dividers;

		gate->clk_hw_dividers = devm_kcalloc(dev, gate_data->num_dividers,
				sizeof(*gate->clk_hw_dividers), GFP_KERNEL);

		for (divider_index = 0; divider_index < gate_data->num_dividers; divider_index++) {
			unsigned int divider = gate_data->dividers[divider_index];

			divider_name = devm_kasprintf(dev, GFP_KERNEL, "%s_div_%u",
					gate_clk_init.name, divider);
			if (!divider_name)
				return -ENOMEM;
			gate->clk_hw_dividers[divider_index] = devm_clk_hw_register_fixed_factor(dev, divider_name,
					gate_clk_init.name, 0, 1, divider);
			if (IS_ERR(gate->clk_hw_dividers[divider_index]))
				return PTR_ERR(gate->clk_hw_dividers[divider_index]);
		}

no_dividers:
		devm_kfree(dev, gate_clk_init.name);
	}

	clkgen->muxes = msc313_mux_register_muxes(dev, regmap, match_data,
			msc313_clkgen_fill_mux_clk_parent_data, clkgen);
	if (IS_ERR(clkgen->muxes))
		return PTR_ERR(clkgen->muxes);

	return devm_of_clk_add_hw_provider(dev, msc313_clkgen_xlate, clkgen);
}

static struct platform_driver msc313_clkgen_driver = {
	.driver = {
		.name = "msc313-clkgen",
		.of_match_table = msc313_clkgen_ids,
	},
	.probe = msc313_clkgen_probe,
};
builtin_platform_driver(msc313_clkgen_driver);
