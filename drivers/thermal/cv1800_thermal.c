// SPDX-License-Identifier: GPL-2.0
/*
 * Temperature sensor driver for the Sophgo CV1800B/SG2000 SoCs.
 *
 * The sensor block reports a 13-bit raw value per channel.  The driver
 * converts channel 0 to milli-Celsius with the vendor's curve-fitted
 * formula and exposes it as a thermal zone; the zone trip points are
 * programmed into the sensor's high/low threshold registers and the
 * threshold interrupt is used to update the zone.
 *
 * Copyright (C) 2021 Sophgo Inc.
 * Copyright (C) 2024 Haylen Chu <heylenay@xxxxxxx>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define TEMPSEN_CTRL			0x04
#define   TEMPSEN_CTRL_EN		BIT(0)
#define   TEMPSEN_CTRL_SEL		GENMASK(7, 4)
#define TEMPSEN_SET			0x0c
#define   TEMPSEN_SET_CHOPSEL		GENMASK(5, 4)
#define   TEMPSEN_SET_ACCSEL		GENMASK(7, 6)
#define   TEMPSEN_SET_CYC_CLKDIV	GENMASK(15, 8)
#define TEMPSEN_INTR_EN			0x10
#define TEMPSEN_INTR_CLR		0x14
#define TEMPSEN_INTR_RAW		0x1c
#define   TEMPSEN_INTR_TH_HIGH(n)	BIT(4 + (n))
#define   TEMPSEN_INTR_TH_LOW(n)	BIT(8 + (n))
#define TEMPSEN_RESULT(n)		(0x20 + (n) * 4)
#define   TEMPSEN_RESULT_MASK		GENMASK(12, 0)
#define   TEMPSEN_RESULT_CLEAR_MAX	BIT(31)
#define TEMPSEN_THRESHOLD(n)		(0x40 + (n) * 4)
#define   TEMPSEN_THRESHOLD_HIGH	GENMASK(15, 0)
#define   TEMPSEN_THRESHOLD_LOW		GENMASK(31, 16)
#define TEMPSEN_AUTO_PERIOD		0x64
#define   TEMPSEN_AUTO_PERIOD_CYCLE	GENMASK(23, 0)

/*
 * Conversion from the 13-bit result:
 *
 *   T (milli-Celsius) = result * 716 * 1000 / 2048 - 273000
 *
 * where 2048 is the number of ticks accumulated per sample.
 */
#define CV1800_TEMP_MULTIPLIER		716
#define CV1800_TEMP_DIVIDER		2048
#define CV1800_TEMP_OFFSET_MC		273000

/* The threshold registers hold a 13-bit result value. */
#define CV1800_TEMP_MIN_MC		(-CV1800_TEMP_OFFSET_MC)
#define CV1800_TEMP_MAX_MC		200000

/*
 * Sensor configuration: 1024-tick chopping, 2048-tick accumulation and
 * a cycle divider of 0x31; the auto-sample period counter is clocked at
 * 1 us, so 1000000 gives one sample per second.
 */
#define CV1800_CHOPSEL_1024T		3
#define CV1800_ACCSEL_2048T		2
#define CV1800_CYCLE_CLKDIV		0x31
#define CV1800_SAMPLE_PERIOD		1000000

struct cv1800_thermal_zone {
	void __iomem *base;
	struct clk *clk_tempsen;
	struct thermal_zone_device *tz;
};

static void cv1800_thermal_init(struct cv1800_thermal_zone *ctz)
{
	u32 reg;

	/* Clear stale threshold interrupts and the latched maximum result. */
	writel(readl(ctz->base + TEMPSEN_INTR_RAW), ctz->base + TEMPSEN_INTR_CLR);
	writel(TEMPSEN_RESULT_CLEAR_MAX, ctz->base + TEMPSEN_RESULT(0));

	reg = readl(ctz->base + TEMPSEN_SET);
	reg &= ~(TEMPSEN_SET_CHOPSEL | TEMPSEN_SET_ACCSEL |
		 TEMPSEN_SET_CYC_CLKDIV);
	reg |= FIELD_PREP(TEMPSEN_SET_CHOPSEL, CV1800_CHOPSEL_1024T) |
	       FIELD_PREP(TEMPSEN_SET_ACCSEL, CV1800_ACCSEL_2048T) |
	       FIELD_PREP(TEMPSEN_SET_CYC_CLKDIV, CV1800_CYCLE_CLKDIV);
	writel(reg, ctz->base + TEMPSEN_SET);

	reg = readl(ctz->base + TEMPSEN_AUTO_PERIOD);
	reg &= ~TEMPSEN_AUTO_PERIOD_CYCLE;
	reg |= FIELD_PREP(TEMPSEN_AUTO_PERIOD_CYCLE, CV1800_SAMPLE_PERIOD);
	writel(reg, ctz->base + TEMPSEN_AUTO_PERIOD);

	/* Sample channel 0 continuously. */
	reg = readl(ctz->base + TEMPSEN_CTRL);
	reg &= ~TEMPSEN_CTRL_SEL;
	reg |= FIELD_PREP(TEMPSEN_CTRL_SEL, 1) | TEMPSEN_CTRL_EN;
	writel(reg, ctz->base + TEMPSEN_CTRL);
}

static void cv1800_thermal_deinit(struct cv1800_thermal_zone *ctz)
{
	u32 reg;

	writel(0, ctz->base + TEMPSEN_INTR_EN);

	reg = readl(ctz->base + TEMPSEN_CTRL);
	reg &= ~(TEMPSEN_CTRL_SEL | TEMPSEN_CTRL_EN);
	writel(reg, ctz->base + TEMPSEN_CTRL);
}

static int cv1800_result_to_mc(u32 result)
{
	return (int)(((u64)result * CV1800_TEMP_MULTIPLIER * 1000) /
		     CV1800_TEMP_DIVIDER) - CV1800_TEMP_OFFSET_MC;
}

static u32 cv1800_mc_to_result(int temp)
{
	return (u32)(((u64)(temp + CV1800_TEMP_OFFSET_MC) *
		      CV1800_TEMP_DIVIDER) /
		     (CV1800_TEMP_MULTIPLIER * 1000));
}

static int cv1800_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct cv1800_thermal_zone *ctz = thermal_zone_device_priv(tz);
	u32 result;

	result = readl(ctz->base + TEMPSEN_RESULT(0)) & TEMPSEN_RESULT_MASK;
	*temp = cv1800_result_to_mc(result);

	return 0;
}

static int cv1800_thermal_set_trips(struct thermal_zone_device *tz, int low,
				    int high)
{
	struct cv1800_thermal_zone *ctz = thermal_zone_device_priv(tz);
	u32 reg;

	low = clamp(low, CV1800_TEMP_MIN_MC, CV1800_TEMP_MAX_MC);
	high = clamp(high, CV1800_TEMP_MIN_MC, CV1800_TEMP_MAX_MC);

	reg = FIELD_PREP(TEMPSEN_THRESHOLD_LOW, cv1800_mc_to_result(low)) |
	      FIELD_PREP(TEMPSEN_THRESHOLD_HIGH, cv1800_mc_to_result(high));
	writel(reg, ctz->base + TEMPSEN_THRESHOLD(0));

	return 0;
}

static const struct thermal_zone_device_ops cv1800_thermal_ops = {
	.get_temp = cv1800_thermal_get_temp,
	.set_trips = cv1800_thermal_set_trips,
};

static irqreturn_t cv1800_thermal_irq(int irq, void *data)
{
	struct cv1800_thermal_zone *ctz = data;

	/* Clear the threshold violations reported by the sensor. */
	writel(readl(ctz->base + TEMPSEN_INTR_RAW), ctz->base + TEMPSEN_INTR_CLR);
	thermal_zone_device_update(ctz->tz, THERMAL_EVENT_UNSPECIFIED);

	return IRQ_HANDLED;
}

static int cv1800_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cv1800_thermal_zone *ctz;
	struct thermal_zone_device *tz;
	int irq, ret;

	ctz = devm_kzalloc(dev, sizeof(*ctz), GFP_KERNEL);
	if (!ctz)
		return -ENOMEM;

	ctz->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctz->base))
		return dev_err_probe(dev, PTR_ERR(ctz->base),
				     "failed to map the sensor registers\n");

	ctz->clk_tempsen = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(ctz->clk_tempsen))
		return dev_err_probe(dev, PTR_ERR(ctz->clk_tempsen),
				     "failed to get the TEMPSEN clock\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	cv1800_thermal_init(ctz);

	tz = devm_thermal_of_zone_register(dev, 0, ctz, &cv1800_thermal_ops);
	if (IS_ERR(tz))
		return dev_err_probe(dev, PTR_ERR(tz),
				     "failed to register the thermal zone\n");
	ctz->tz = tz;

	ret = devm_request_threaded_irq(dev, irq, NULL, cv1800_thermal_irq,
					IRQF_ONESHOT, "cv1800_thermal", ctz);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq %d\n", irq);

	/* The trip thresholds were programmed by the zone registration. */
	writel(TEMPSEN_INTR_TH_LOW(0) | TEMPSEN_INTR_TH_HIGH(0),
	       ctz->base + TEMPSEN_INTR_EN);

	platform_set_drvdata(pdev, ctz);

	return 0;
}

static void cv1800_thermal_remove(struct platform_device *pdev)
{
	cv1800_thermal_deinit(platform_get_drvdata(pdev));
}

static const struct of_device_id cv1800_thermal_of_match[] = {
	{ .compatible = "sophgo,cv1800b-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, cv1800_thermal_of_match);

static struct platform_driver cv1800_thermal_driver = {
	.probe = cv1800_thermal_probe,
	.remove = cv1800_thermal_remove,
	.driver = {
		.name = "cv1800-thermal",
		.of_match_table = cv1800_thermal_of_match,
	},
};
module_platform_driver(cv1800_thermal_driver);

MODULE_DESCRIPTION("Sophgo CV1800B/SG2000 thermal sensor driver");
MODULE_AUTHOR("Haylen Chu <heylenay@xxxxxxx>");
MODULE_LICENSE("GPL");
