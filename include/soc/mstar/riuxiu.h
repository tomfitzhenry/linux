/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MStar/SigmaStar RIU and XIU bus accessors.
 *
 * Ported from linux-chenxing/mstar_v6_7_rebase ("include/soc/mstar/riuxiu.h":
 * https://github.com/linux-chenxing/linux/blob/mstar_v6_7_rebase/include/soc/mstar/riuxiu.h)
 * and adapted for the SSC335 (Infinity6B0): the RIU on that die drops plain
 * 16-bit stores to a halfword address on real silicon (the M1 hardware work
 * found this while bringing up the blind console), while U-Boot and the
 * vendor kernel always issue 32-bit stores for the 16-bit register payload.
 * A 32-bit store to the low half's address therefore carries the low half
 * value; the upper two bytes of the 4-byte slot are padding the RIU ignores.
 * Reads are plain 16-bit loads of each half (proven on hardware in M1).
 */

#ifndef _SOC_MSTAR_RIUXIU_H_
#define _SOC_MSTAR_RIUXIU_H_

#include <linux/io.h>

/*
 * RIU accessors.
 *
 * These are for accessing split 32bit registers in MStar and
 * third party IPs.
 *
 * To access a split 32bit register in an MStar IP use
 * the _abs() version with the full address for the lower
 * half of the register.
 *
 * To access a split 32bit register for a third party IP where
 * the offset from the start of the registers needs to be
 * doubled to get the right address use the non-abs version.
 */
static inline u32 riu_readl_abs(void __iomem *base)
{
	return readw(base + 4) << 16 | readw(base);
}

static inline u32 riu_readl(void __iomem *base, unsigned int offset)
{
	return riu_readl_abs(base + (offset * 2));
}

static inline u32 riu_readl_relaxed_abs(void __iomem *base)
{
	return readw_relaxed(base + 4) << 16 | readw_relaxed(base);
}

static inline u32 riu_readl_relaxed(void __iomem *base, unsigned int offset)
{
	return riu_readl_relaxed_abs(base + (offset * 2));
}

/*
 * Do not change this order.  For the EMAC at least the write order must be
 * the lower half and then the upper half, otherwise the command side effects
 * (transmit, etc.) do not fire.
 *
 * The stores are deliberately 32-bit (writel), not 16-bit (writew): on the
 * SSC335 the RIU drops plain 16-bit stores to the halfword address (M1
 * hardware finding, matching U-Boot's 32-bit str "payload" stores).
 */
static inline void riu_writel_relaxed_abs(void __iomem *base, u32 value)
{
	writel_relaxed(value, base);
	writel_relaxed(value >> 16, base + 4);
}

static inline void riu_writel_relaxed(void __iomem *base, unsigned int offset, u32 value)
{
	riu_writel_relaxed_abs(base + (offset * 2), value);
}

static inline void riu_writel_abs(void __iomem *base, u32 value)
{
	writel(value, base);
	writel(value >> 16, base + 4);
}

static inline void riu_writel(void __iomem *base, unsigned int offset, u32 value)
{
	riu_writel_abs(base + (offset * 2), value);
}

/*
 * XIU accessors, use these whenever possible for third party IPs.
 * For MStar IPs just use plain readl(), writel() etc.
 */

static inline u32 xiu_readl(void __iomem *base, unsigned int offset)
{
	return readl(base + (offset * 2));
}

static inline u32 xiu_readl_relaxed(void __iomem *base, unsigned int offset)
{
	return readl_relaxed(base + (offset * 2));
}

static inline void xiu_writel(void __iomem *base, unsigned int offset, u32 value)
{
	writel(value, base + (offset * 2));
}

static inline void xiu_writel_relaxed(void __iomem *base, unsigned int offset, u32 value)
{
	writel_relaxed(value, base + (offset * 2));
}

#endif
