// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the MStar/SigmaStar MSC313 and MSC313E internal Ethernet PHY
 * (the "Albany" 10/100 PHY), including the Infinity6B0/SSC335 die.
 *
 * The PHY itself is MDIO-accessible through the EMAC's management port, but
 * its analog front end (LDO, SADC, ADCPL, REF, TX drivers, clock selection)
 * is controlled through a separate register block in the SoC's RIU
 * ("PHYANA"), which the PHY driver reaches through the mstar,phyana syscon
 * phandle on the PHY node.  On boards where the bootloader has already
 * brought the PHY up (every TFTP boot does) the power-up sequence is only
 * needed for resume; it is the vendor 4.9 kernel's sequence.
 *
 * Ported from linux-chenxing/mstar_v6_7_rebase drivers/net/phy/msc313.c:
 * https://github.com/linux-chenxing/linux/blob/mstar_v6_7_rebase/drivers/net/phy/msc313.c
 * and cross-checked against the vendor 4.9 tree
 * openipc/sigmastar-infinity6b0 drivers/sstar/emac/hal/infinity6b0/mhal_emac.c
 * (_MHal_EMAC_albany_clk_on/_off) and drivers/sstar/netphy/sstar_100_phy.c
 * (which reports PHY ID 0x11112222 for the SSC335's internal PHY).
 */
#include <linux/ethtool.h>
#include <linux/export.h>
#include <linux/mdio.h>
#include <linux/mfd/syscon.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/regmap.h>

#include "msc313.h"

#define MSC313_PHY_ID		0xdeadbeef
#define MSC313E_PHY_ID		0xdeadb33f
/* The SSC335's internal "Albany" PHY reports this over MDIO. */
#define SSTAR_100_PHY_ID	0x11112222
#define MSC313_PHY_MASK		0xffffffff

struct msc313_phy_data;

struct msc313_phy_priv {
	struct regmap *pmsleep;
	struct regmap *phyana;
	const struct msc313_phy_data *data;
	struct msc313e_fields msc313e_fields;
	bool poweredup;
};

struct msc313_phy_data {
	void (*powerup)(struct msc313_phy_priv *priv);
	void (*powerdown)(struct msc313_phy_priv *priv);
};

static void msc313_powerdown(struct msc313_phy_priv *priv)
{
	regmap_write(priv->phyana, REG_LDO, 0xffff);
}

static void msc313_powerup(struct msc313_phy_priv *priv)
{
}

static const struct msc313_phy_data msc313_data = {
	.powerup = msc313_powerup,
	.powerdown = msc313_powerdown,
};

static void msc313e_powerup(struct msc313_phy_priv *priv)
{
	struct msc313e_fields *f = &priv->msc313e_fields;

	if (priv->poweredup)
		return;

	regmap_field_write(f->anarst, 1);
	mdelay(100);
	regmap_field_write(f->anarst, 0);
	mdelay(100);

	/* Power-on LDO */
	regmap_write(priv->phyana, REG_LDO, 0x0000);
	/* Power-on SADC */
	regmap_field_write(f->sadcpd, 0);
	/* Power-on ADCPL */
	regmap_field_write(f->adcplpd, 0);
	/* Power-on REF */
	regmap_field_write(f->refpd, 0);
	/* Power-on TX */
	regmap_field_write(f->txpd1, 0);
	/* Power-on TX */
	regmap_field_write(f->txpd2, 0);
	/* CLKO_ADC_SEL */
	regmap_field_write(f->clkoadcsel, 1);
	/* reg_adc_clk_select */
	regmap_field_write(f->adcclksel, 1);
	/* 100gat */
	regmap_field_write(f->hundredgat, 0);
	/* 200gat */
	regmap_field_write(f->twohundredgat, 0);

	priv->poweredup = true;
}

static void msc313e_powerdown(struct msc313_phy_priv *priv)
{
	struct msc313e_fields *f = &priv->msc313e_fields;

	if (!priv->poweredup)
		return;

	regmap_field_write(f->anarst, 1);
	/* Power-off LDO */
	regmap_write(priv->phyana, REG_LDO, 0x0102);
	/* Power-off SADC */
	regmap_field_write(f->sadcpd, ~0);
	/* Power-off ADCPL */
	regmap_field_write(f->adcplpd, ~0);
	/* Power-off REF */
	regmap_field_write(f->refpd, ~0);
	/* Power-off TX */
	regmap_field_write(f->txpd1, ~0);
	/* Power-off TX */
	regmap_field_write(f->txpd2, ~0);

	priv->poweredup = false;
}

static const struct msc313_phy_data msc313e_data = {
	.powerup = msc313e_powerup,
	.powerdown = msc313e_powerdown,
};

static int msc313_phy_suspend(struct phy_device *phydev)
{
	struct msc313_phy_priv *priv = phydev->priv;

	priv->data->powerdown(priv);
	return 0;
}

static int msc313_phy_resume(struct phy_device *phydev)
{
	struct msc313_phy_priv *priv = phydev->priv;

	priv->data->powerup(priv);
	return 0;
}

static int msc313_phy_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct device_node *of_node = dev->of_node;
	struct msc313_phy_priv *priv;
	struct msc313e_fields *f;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pmsleep = syscon_regmap_lookup_by_phandle(of_node, "mstar,pmsleep");
	if (IS_ERR(priv->pmsleep))
		return dev_err_probe(dev, PTR_ERR(priv->pmsleep),
				     "failed to get the pmsleep syscon\n");

	priv->phyana = syscon_regmap_lookup_by_phandle(of_node, "mstar,phyana");
	if (IS_ERR(priv->phyana))
		return dev_err_probe(dev, PTR_ERR(priv->phyana),
				     "failed to get the phyana syscon\n");

	f = &priv->msc313e_fields;
	f->anarst = devm_regmap_field_alloc(dev, priv->phyana, anarst);
	f->sadcpd = devm_regmap_field_alloc(dev, priv->phyana, sadcpd);
	f->adcplpd = devm_regmap_field_alloc(dev, priv->phyana, adcplpd);
	f->refpd = devm_regmap_field_alloc(dev, priv->phyana, refpd);
	f->txpd1 = devm_regmap_field_alloc(dev, priv->phyana, txpd1);
	f->txpd2 = devm_regmap_field_alloc(dev, priv->phyana, txpd2);
	f->clkoadcsel = devm_regmap_field_alloc(dev, priv->phyana, clkoadcsel);
	f->adcclksel = devm_regmap_field_alloc(dev, priv->phyana, adcclksel);
	f->hundredgat = devm_regmap_field_alloc(dev, priv->phyana, hundredgat);
	f->twohundredgat = devm_regmap_field_alloc(dev, priv->phyana, twohundredgat);
	if (IS_ERR(f->anarst) || IS_ERR(f->sadcpd) || IS_ERR(f->adcplpd) ||
	    IS_ERR(f->refpd) || IS_ERR(f->txpd1) || IS_ERR(f->txpd2) ||
	    IS_ERR(f->clkoadcsel) || IS_ERR(f->adcclksel) ||
	    IS_ERR(f->hundredgat) || IS_ERR(f->twohundredgat)) {
		ret = -ENOMEM;
		return ret;
	}

	priv->data = phydev->drv->driver_data;
	phydev->priv = priv;

	return 0;
}

static struct phy_driver msc313_driver[] = {
	{
		.phy_id		= SSTAR_100_PHY_ID,
		.phy_id_mask	= MSC313_PHY_MASK,
		.name		= "SigmaStar SSC335 internal PHY",
		.features	= PHY_BASIC_FEATURES,
		.probe		= msc313_phy_probe,
		.suspend	= msc313_phy_suspend,
		.resume		= msc313_phy_resume,
		.driver_data	= &msc313e_data,
	},
	{
		.phy_id		= MSC313E_PHY_ID,
		.phy_id_mask	= MSC313_PHY_MASK,
		.name		= "MStar MSC313E internal PHY",
		.features	= PHY_BASIC_FEATURES,
		.probe		= msc313_phy_probe,
		.suspend	= msc313_phy_suspend,
		.resume		= msc313_phy_resume,
		.driver_data	= &msc313e_data,
	},
	{
		.phy_id		= MSC313_PHY_ID,
		.phy_id_mask	= MSC313_PHY_MASK,
		.name		= "MStar MSC313 internal PHY",
		.features	= PHY_BASIC_FEATURES,
		.probe		= msc313_phy_probe,
		.suspend	= msc313_phy_suspend,
		.resume		= msc313_phy_resume,
		.driver_data	= &msc313_data,
	},
};
module_phy_driver(msc313_driver);

static struct mdio_device_id __maybe_unused msc313_tbl[] = {
	{ SSTAR_100_PHY_ID, MSC313_PHY_MASK },
	{ MSC313E_PHY_ID, MSC313_PHY_MASK },
	{ MSC313_PHY_ID, MSC313_PHY_MASK },
	{ }
};

MODULE_DEVICE_TABLE(mdio, msc313_tbl);

MODULE_DESCRIPTION("MStar MSC313/MSC313E internal Ethernet PHY driver");
MODULE_LICENSE("GPL");
