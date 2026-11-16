// SPDX-License-Identifier: GPL-2.0
/*
 * SigmaStar SSC335 (Infinity6B0) "blind console": a UDP early console.
 *
 * The TP-Link Tapo C212 v2 (SSC335, single Cortex-A7) has no exposed UART,
 * so the only way to watch the kernel boot is to hand the console output to
 * the Ethernet MAC that the bootloader already brought up.  U-Boot runs
 * tftpboot immediately before bootm; when it finishes it disables only the
 * transmitter and receiver (clears EMAC_TE/EMAC_RE in REG_ETH_CTL) and
 * leaves the MAC clocks, the MAC address, the JULIAN block and the internal
 * "Albany" 100BASE-TX PHY link alive.  The earlycon therefore does not
 * initialise the PHY: it re-enables the transmitter, writes one complete
 * Ethernet + IPv4 + UDP frame per console write into a reserved data page
 * and asks the MAC's single-packet TX slot to send it.
 *
 * The MAC is used through its 32-bit "X32" view at 0x1f343c00, where
 * register G of the AT91RM9200-style register set sits at 0x1f343c00 + 2*G:
 *
 *   CTL  0x00   TAR 0x18   TCR 0x20   TSR 0x28   IDR 0x58
 *
 * (U-Boot uses a 16-bit RIU view of the same registers; the X32 view is
 * what the vendor kernel uses, so it is what this driver uses too.)
 *
 * The transmit path is: format the frame in DRAM, flush the SigmaStar
 * "L3 bridge" AXI->MIU write pipe so the MAC's DMA engine cannot read stale
 * data, wait for the TX FIFO slot to go idle, then write the frame's MIU0
 * bus address to TAR and its length to TCR.  CPU physical memory starts at
 * 0x20000000 while every SigmaStar DMA master sees DDR2 from 0, hence the
 * fixed -0x20000000 conversion below (the DT has no dma-ranges window that
 * would apply to a raw earlycon write).
 *
 * Memory management this early is a problem: an earlycon is registered from
 * an early_param() handler while the ARM setup_arch() is still running, i.e.
 * long before the page allocator, the vmalloc area and therefore ioremap()
 * exist, and console write() can be called from any context (including
 * before SMP and from panic paths), so it may not sleep or allocate either.
 * The driver therefore uses the earlycon fixmap slot to map whichever of
 * its three pages it needs, one page at a time, for as long as it has to.
 * Once the mm is up, an early_initcall replaces the fixmap remapping with
 * ordinary uncached ioremap() mappings (when the DT has reserved the frame
 * page no-map, as the C212 DTS does, so that ioremap() is allowed on it).
 *
 * This is a best-effort debug facility: it inherits the MAC state that the
 * bootloader left behind, has no flow control or retransmission, and is
 * only as reliable as that bootloader hand-off.  It is selected with
 * "earlycon=sstar_udp" (an explicit address is accepted as an override of
 * the MAC base, e.g. the boot.scr's "earlycon=sstar_udp,0x1f343c00").
 */

#include <linux/console.h>
#include <linux/errno.h>
#include <linux/hex.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/processor.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/barrier.h>
#include <asm/fixmap.h>
#include <asm/page.h>

/* Ethernet II + IPv4 + UDP, no payload. */
#define SSTAR_UDP_HDR_LEN		42
/* Keep the frame below a 1500 byte MTU once the headers are added. */
#define SSTAR_UDP_MAX_PAYLOAD		1400
/* Ethernet minimum frame size, FCS excluded. */
#define SSTAR_UDP_MIN_FRAME		60
#define SSTAR_UDP_MAC_LEN		6
#define SSTAR_UDP_IP_LEN		4

/* EMAC X32 view offsets: 32-bit register G is at 2 * G. */
#define SSTAR_EMAC_CTL			0x00
#define SSTAR_EMAC_CFG			0x08
#define SSTAR_EMAC_SR			0x10
#define SSTAR_EMAC_RSR_ADDR		0x40
#define SSTAR_EMAC_TAR			0x18
#define SSTAR_EMAC_TCR			0x20
#define SSTAR_EMAC_TSR			0x28
#define SSTAR_EMAC_ISR			0x48
#define SSTAR_EMAC_IMR			0x60
#define SSTAR_EMAC_IDR			0x58

/* The internal Albany PHY is directly readable in RIU banks 0x31..0x33. */
#define SSTAR_PHY_BASE			0x1f006200

/*
 * The EMAC's 16-bit RIU view (what the vendor driver and U-Boot use).  Each
 * 32-bit register G is two 16-bit halves at +2G and +2G+4; the MStar "EMAC1"
 * bank sits at +0x200 and the JULIAN extras start at +0x208.
 */
#define SSTAR_EMAC_RIU_BASE		0x1f2a2000
#define SSTAR_EMAC1_BANK		0x200
#define SSTAR_EMAC1_TXQ_USED		(SSTAR_EMAC1_BANK + 2 * 0x24)
#define SSTAR_EMAC1_TXQ_ENABLE		(SSTAR_EMAC1_BANK + 2 * 0x24 + 1)
#define SSTAR_EMAC1_TXQ_EN_BIT		BIT(7)
/* JULIAN register offsets within the EMAC RIU mapping. */
#define SSTAR_JULIAN_0100		(2 * 0x100)
#define SSTAR_JULIAN_0104		(2 * 0x104)
#define SSTAR_JULIAN_0108		(2 * 0x108)
#define SSTAR_JULIAN_0138		(2 * 0x138)
#define SSTAR_JULIAN_013C		(2 * 0x13c)
#define SSTAR_JULIAN_SW_DESC_ENABLE	BIT(0)

#define SSTAR_EMAC_CTL_TE		BIT(3)
/* TSR bits: buffer not queued and transmit idle. */
#define SSTAR_EMAC_TSR_IDLE		0x18

/* SigmaStar "L3 bridge": flush the AXI->MIU write pipe before DMA. */
#define SSTAR_L3BRIDGE_FLUSH		0x14
#define SSTAR_L3BRIDGE_STATUS		0x40
#define SSTAR_L3BRIDGE_DONE		BIT(12)
#define SSTAR_L3BRIDGE_FLUSH_1		0x10
#define SSTAR_L3BRIDGE_FLUSH_2		0x11

/* CPU physical memory base: DMA bus addresses are physical - this. */
#define SSTAR_DMA_OFFSET		0x20000000UL

/*
 * M1 bring-up black box: the board has no UART, so when a hardware boot
 * misbehaves the only affordable diagnostic is to leave the MAC/PHY register
 * state in a reserved DRAM page and read it back over SSH from the OpenIPC
 * fallback boot (busybox devmem 0x23ff2000).  It sits between the frame page
 * (0x23ff3000) and above U-Boot's 0x23f20000..0x23fef460 relocation, i.e. in
 * DRAM the IPL's MIU BIST does not wipe.  Cheap: a handful of word stores per
 * frame, disabled with SERIAL_EARLYCON_SSSTAR_UDP_BLACKBOX=n.
 */
/* Word layout of the black box page (compiled in either way: the writer is a
 * no-op unless CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_BLACKBOX is set). */
enum sstar_udp_bb_word {
	SSTAR_BB_MAGIC_W = 0,
	SSTAR_BB_VERSION_W,
	SSTAR_BB_EMAC_PHYS_W,
	SSTAR_BB_EMAC_OFF_W,
	SSTAR_BB_IOMAPPED_W,
	SSTAR_BB_CTL_PRE_W,
	SSTAR_BB_CFG_W,
	SSTAR_BB_SR_W,
	SSTAR_BB_TSR_PRE_W,
	SSTAR_BB_ISR_W,
	SSTAR_BB_IMR_W,
	SSTAR_BB_IDR_W,
	SSTAR_BB_CTL_POST_W,
	SSTAR_BB_PHY_BMSR_W,
	SSTAR_BB_PHY_BMCR_W,
	SSTAR_BB_L3_STATUS16_W,
	SSTAR_BB_L3_STATUS32_W,
	SSTAR_BB_FRAMES_W,
	SSTAR_BB_TSR_BEFORE_W,
	SSTAR_BB_TSR_AFTER_W,
	SSTAR_BB_LAST_LEN_W,
	SSTAR_BB_FRAME_W0_W,
	SSTAR_BB_FRAME_W1_W,
	SSTAR_BB_REMAP_DONE_W,
	SSTAR_BB_CTL_LATE_W,
	SSTAR_BB_TSR_LATE_W,
	SSTAR_BB_SR_LATE_W,
	SSTAR_BB_PHY_BMSR_LATE_W,
	SSTAR_BB_IMR_LATE_W,
	SSTAR_BB_TXQ_EN_PRE_W,
	SSTAR_BB_TXQ_EN_POST_W,
	SSTAR_BB_TXQ_USED_PRE_W,
	SSTAR_BB_TXQ_USED_POST_W,
	SSTAR_BB_JULIAN_104_PRE_W,
	SSTAR_BB_JULIAN_104_POST_W,
	SSTAR_BB_TXQ_USED_LATE_W,
	SSTAR_BB_X32_CTL_LATE_W,
	SSTAR_BB_JULIAN_0100_W,
	SSTAR_BB_JULIAN_0108_W,
	SSTAR_BB_JULIAN_0138_W,
	SSTAR_BB_JULIAN_013C_W,
	SSTAR_BB_TXQ_WORD_PRE_W,
	SSTAR_BB_TXQ_WORD_POST_W,
	SSTAR_BB_SA2L_RIU_W,
	SSTAR_BB_CTL_READW_W,
	SSTAR_BB_X32_JULIAN104_PRE_W,
	SSTAR_BB_X32_JULIAN104_POST_W,
	SSTAR_BB_X32_XTSR_PRE_W,
	SSTAR_BB_X32_XTSR_POST_W,
	SSTAR_BB_CTL_RIU_POST_W,
	SSTAR_BB_X32_TAR_POST_W,
	SSTAR_BB_LOOP_DESC0_W,
	SSTAR_BB_LOOP_DESC1_W,
	SSTAR_BB_LOOP_RXBUF0_W,
	SSTAR_BB_LOOP_RXBUF1_W,
	SSTAR_BB_LOOP_RXBUF2_W,
	SSTAR_BB_LOOP_RSR_W,
	SSTAR_BB_LOOP_CTL_W,
};

#ifdef CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_BLACKBOX
#define SSTAR_BB_PHYS			CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_BLACKBOX_PHYS
#define SSTAR_BB_MAGIC			0x32313243U	/* "C212" LE */
#define SSTAR_BB_WORDS			64

static void __iomem *sstar_udp_bb;
static u32 sstar_udp_bb_early[SSTAR_BB_WORDS];
static bool sstar_udp_bb_early_valid;

/*
 * ioremap() is not available while setup() runs (see sstar_udp_try_ioremap()),
 * so samples taken that early are buffered in .init.data and replayed when
 * the early_initcall installs the permanent mapping.
 */
static void sstar_udp_bb_put(unsigned int idx, u32 val)
{
	if (idx >= SSTAR_BB_WORDS)
		return;
	if (sstar_udp_bb) {
		writel_relaxed(val, sstar_udp_bb + idx * 4);
	} else {
		sstar_udp_bb_early[idx] = val;
		sstar_udp_bb_early_valid = true;
	}
}

static void __init sstar_udp_bb_map(void)
{
	unsigned int i;

	if (sstar_udp_bb)
		return;
	sstar_udp_bb = ioremap(SSTAR_BB_PHYS, PAGE_SIZE);
	if (!sstar_udp_bb)
		return;
	memset_io(sstar_udp_bb, 0, PAGE_SIZE);
	writel_relaxed(SSTAR_BB_MAGIC, sstar_udp_bb + SSTAR_BB_MAGIC_W * 4);
	writel_relaxed(1, sstar_udp_bb + SSTAR_BB_VERSION_W * 4);
	if (!sstar_udp_bb_early_valid)
		return;
	for (i = 0; i < SSTAR_BB_WORDS; i++) {
		if (i == SSTAR_BB_MAGIC_W || i == SSTAR_BB_VERSION_W)
			continue;
		if (sstar_udp_bb_early[i])
			writel_relaxed(sstar_udp_bb_early[i],
				       sstar_udp_bb + i * 4);
	}
}
#else
static void sstar_udp_bb_put(unsigned int idx, u32 val) { }
static void __init sstar_udp_bb_map(void) { }
#endif

/* Bounded spins: a broken MAC block must never hang the boot console. */
#define SSTAR_POLL_LIMIT		1000000

struct sstar_udp_cfg {
	u8 src_mac[SSTAR_UDP_MAC_LEN];
	u8 src_ip[SSTAR_UDP_IP_LEN];
	u8 dst_ip[SSTAR_UDP_IP_LEN];
	u16 src_port;
	u16 dst_port;
};

static struct sstar_udp_cfg sstar_udp_cfg;
static unsigned long sstar_udp_emac_phys =
	CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_EMAC_BASE;
static unsigned long sstar_udp_emac_off;
static const unsigned long sstar_udp_frame_phys =
	CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_FRAME_PHYS;
static u16 sstar_udp_ip_id;
static bool sstar_udp_enabled;

/* Steady-state mappings taken over by sstar_udp_remap() (early_initcall). */
static bool sstar_udp_iomapped;
static void __iomem *sstar_udp_frame_mapped;
static void __iomem *sstar_udp_l3_mapped;
static void __iomem *sstar_udp_emac_mapped;
static void __iomem *sstar_udp_riu_mapped;

/*
 * Internet checksum helpers.  The frame layout is fixed and the header is
 * a byte array, so summing bytewise is both endian independent and safe on
 * the unaligned payload that printk hands us.
 */
static u32 sstar_udp_csum_partial(u32 sum, const void *data, unsigned int len)
{
	const u8 *p = data;

	while (len > 1) {
		sum += ((u32)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (u32)p[0] << 8;

	return sum;
}

static u16 sstar_udp_csum_fold(u32 sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (u16)~sum;
}

/*
 * Build the Ethernet + IPv4 + UDP header for one frame.  The UDP checksum
 * is computed over the pseudo header and the payload; a computed value of
 * zero is transmitted as 0xffff, as IPv4 requires.
 */
static void sstar_udp_build_hdr(const struct sstar_udp_cfg *cfg, u8 *h,
				const char *payload, unsigned int payload_len,
				u16 ip_id)
{
	unsigned int udp_len = 8 + payload_len;
	unsigned int ip_len = 20 + udp_len;
	u32 sum;
	u16 csum;

	/* Ethernet II: broadcast to the LAN, known camera source address. */
	memset(h, 0xff, 6);
	memcpy(h + 6, cfg->src_mac, SSTAR_UDP_MAC_LEN);
	h[12] = 0x08;
	h[13] = 0x00;

	/* IPv4. */
	h[14] = 0x45;			/* version 4, IHL 5 */
	h[15] = 0x00;			/* DSCP/ECN */
	h[16] = ip_len >> 8;
	h[17] = ip_len & 0xff;
	h[18] = ip_id >> 8;
	h[19] = ip_id & 0xff;
	h[20] = 0x00;			/* flags, no fragmentation */
	h[21] = 0x00;
	h[22] = 64;			/* TTL */
	h[23] = 17;			/* IPPROTO_UDP */
	h[24] = 0x00;			/* checksum filled in below */
	h[25] = 0x00;
	memcpy(h + 26, cfg->src_ip, SSTAR_UDP_IP_LEN);
	memcpy(h + 30, cfg->dst_ip, SSTAR_UDP_IP_LEN);
	csum = sstar_udp_csum_fold(sstar_udp_csum_partial(0, h + 14, 20));
	h[24] = csum >> 8;
	h[25] = csum & 0xff;

	/* UDP. */
	h[34] = cfg->src_port >> 8;
	h[35] = cfg->src_port & 0xff;
	h[36] = cfg->dst_port >> 8;
	h[37] = cfg->dst_port & 0xff;
	h[38] = udp_len >> 8;
	h[39] = udp_len & 0xff;
	h[40] = 0x00;
	h[41] = 0x00;

	/* UDP checksum: pseudo header (IPs, zero, protocol, UDP length). */
	sum = sstar_udp_csum_partial(0, h + 26, 2 * SSTAR_UDP_IP_LEN);
	sum += 17;			/* zero + IPPROTO_UDP */
	sum += udp_len;
	sum = sstar_udp_csum_partial(sum, h + 34, 8);
	sum = sstar_udp_csum_partial(sum, payload, payload_len);
	csum = sstar_udp_csum_fold(sum);
	if (!csum)
		csum = 0xffff;
	h[40] = csum >> 8;
	h[41] = csum & 0xff;
}

/* Map a physical page in the earlycon fixmap slot and return its address. */
static void __iomem *sstar_udp_fixmap(unsigned long phys)
{
	preempt_disable();
	set_fixmap_io(FIX_EARLYCON_MEM_BASE, phys & PAGE_MASK);
	preempt_enable();

	return (void __iomem *)__fix_to_virt(FIX_EARLYCON_MEM_BASE) +
		(phys & ~PAGE_MASK);
}

static void __iomem *sstar_udp_frame_va(void)
{
	if (sstar_udp_iomapped)
		return sstar_udp_frame_mapped;

	return sstar_udp_fixmap(sstar_udp_frame_phys);
}

static void __iomem *sstar_udp_l3_va(void)
{
	if (sstar_udp_iomapped)
		return sstar_udp_l3_mapped;

	return sstar_udp_fixmap(CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_L3BRIDGE_BASE);
}

static void __iomem *sstar_udp_emac_va(void)
{
	if (sstar_udp_iomapped)
		return sstar_udp_emac_mapped;

	return sstar_udp_fixmap(sstar_udp_emac_phys);
}

static void __iomem *sstar_udp_phy_va(void)
{
	return sstar_udp_fixmap(SSTAR_PHY_BASE);
}

/*
 * One fixmap slot serves every early (pre-early_initcall) mapping, so never
 * hold an X32/RIU/PHY/frame pointer across another sstar_udp_*_va() call:
 * map the page again for every access instead.
 */
static u32 sstar_udp_emac_read(unsigned int off)
{
	return readl(sstar_udp_emac_va() + off);
}

static void __iomem *sstar_udp_riu_va(void)
{
	if (sstar_udp_iomapped)
		return sstar_udp_riu_mapped;

	return sstar_udp_fixmap(SSTAR_EMAC_RIU_BASE);
}

static u16 sstar_udp_riu_read16(unsigned int off);
static void sstar_udp_riu_write16(unsigned int off, u16 val);

/*
 * Make sure the MAC's hardware TX queue is enabled.  The vendor driver's
 * MHal_EMAC_Init()/_MHal_EMAC_TXQ_Enable() sets BIT(7) of EMAC1 register
 * 0x25 and sets SOFTWARE_DESCRIPTOR_ENABLE in JULIAN 0x104 before inserting
 * frames with TAR/TCR; U-Boot's eth_halt clears the enable, so the kernel
 * has to put it back or every TAR/TCR write is dropped (the MAC reports
 * transmit underrun/corruption in TSR and nothing reaches the wire).
 */
static void sstar_udp_emac_txq_ping(void)
{
	void __iomem *riu = sstar_udp_riu_va();
	u8 used;
	u16 j;

	used = readb(riu + SSTAR_EMAC1_TXQ_USED) & 0x7f;
	j = readw(riu + SSTAR_JULIAN_0104);

	sstar_udp_bb_put(SSTAR_BB_TXQ_USED_PRE_W, used);
	sstar_udp_bb_put(SSTAR_BB_JULIAN_104_PRE_W, j);
	sstar_udp_bb_put(SSTAR_BB_JULIAN_0100_W,
			 readw(riu + SSTAR_JULIAN_0100));
	sstar_udp_bb_put(SSTAR_BB_JULIAN_0108_W,
			 readw(riu + SSTAR_JULIAN_0108));
	sstar_udp_bb_put(SSTAR_BB_JULIAN_0138_W,
			 readw(riu + SSTAR_JULIAN_0138));
	sstar_udp_bb_put(SSTAR_BB_JULIAN_013C_W,
			 readw(riu + SSTAR_JULIAN_013C));
	sstar_udp_bb_put(SSTAR_BB_TXQ_EN_PRE_W,
			 !!(readb(riu + SSTAR_EMAC1_TXQ_ENABLE) &
			    SSTAR_EMAC1_TXQ_EN_BIT));

	{
		u16 txq = readl(riu + SSTAR_EMAC1_TXQ_USED) & 0xffff;

		sstar_udp_bb_put(SSTAR_BB_TXQ_WORD_PRE_W, txq);
		/* BIT(7) of the high byte = bit 15 of the 16-bit word. */
		writel(txq | 0x8000, riu + SSTAR_EMAC1_TXQ_USED);
		sstar_udp_bb_put(SSTAR_BB_TXQ_WORD_POST_W,
				 readl(riu + SSTAR_EMAC1_TXQ_USED) & 0xffff);
	}
	sstar_udp_riu_write16(SSTAR_JULIAN_0104,
			      j | SSTAR_JULIAN_SW_DESC_ENABLE);

	sstar_udp_bb_put(SSTAR_BB_TXQ_EN_POST_W,
			 !!(readb(riu + SSTAR_EMAC1_TXQ_ENABLE) &
			    SSTAR_EMAC1_TXQ_EN_BIT));
	sstar_udp_bb_put(SSTAR_BB_TXQ_USED_POST_W,
			 readb(riu + SSTAR_EMAC1_TXQ_USED) & 0x7f);
	sstar_udp_bb_put(SSTAR_BB_JULIAN_104_POST_W,
			 readw(riu + SSTAR_JULIAN_0104));
}

/*
 * Flush the L3 AXI->MIU write pipe.  This is the sequence the vendor
 * bootloader and kernel use; without it the MAC may DMA a stale frame on
 * real silicon (QEMU's flat RAM hides the bug).  readl_relaxed() and
 * writel_relaxed() are deliberate: with CONFIG_ARM_HEAVY_MB, writel()
 * itself ends up in the platform's MIU flush routine.
 */
static void sstar_udp_flush_l3(void)
{
	void __iomem *l3 = sstar_udp_l3_va();
	unsigned int i;
	u16 status;

	/*
	 * The vendor (openipc/sigmastar-infinity6b0
	 * arch/arm/mach-sstar/infinity6b0/soc.c:mstar_chip_flush_miu_pipe())
	 * does this with 16-bit accesses: write 0x10 then 0x11 to the flush
	 * register and poll BIT(12) of the status register.  The RIU is a
	 * 16-bit register file, so match that access width exactly rather
	 * than assuming a 32-bit access is equivalent.
	 */
	writew_relaxed(SSTAR_L3BRIDGE_FLUSH_1, l3 + SSTAR_L3BRIDGE_FLUSH);
	writew_relaxed(SSTAR_L3BRIDGE_FLUSH_2, l3 + SSTAR_L3BRIDGE_FLUSH);

	for (i = 0; i < SSTAR_POLL_LIMIT; i++) {
		status = readw_relaxed(l3 + SSTAR_L3BRIDGE_STATUS);
		if (status & SSTAR_L3BRIDGE_DONE)
			break;
		cpu_relax();
	}
	sstar_udp_bb_put(SSTAR_BB_L3_STATUS16_W, status);
	sstar_udp_bb_put(SSTAR_BB_L3_STATUS32_W,
			 readl_relaxed(l3 + SSTAR_L3BRIDGE_STATUS));
}

/* Hand one complete frame to the MAC's single-packet TX slot. */
/*
 * The EMAC's normal registers are accessed through its 16-bit RIU view, the
 * same view U-Boot's own Ethernet driver uses: register G occupies the two
 * 16-bit words at +2G and +2G+4, and the command side effects (transmit,
 * etc.) fire when the high half is written.  The 32-bit "X32" alias at
 * 0x1F343C00 reads back the same state, but writing a command through it was
 * observed to have no effect on the MAC on the real camera (frames counted,
 * TSR changed, nothing on the wire), while U-Boot's RIU writes do transmit.
 */
static u16 sstar_udp_riu_read16(unsigned int off)
{
	return readw(sstar_udp_riu_va() + off);
}

/*
 * U-Boot's own EMAC driver performs the RIU accesses as 32-bit stores to the
 * halfword address (see the str/ldr pair around 0x23e19c54 in the installed
 * U-Boot): a 32-bit store carries the 16-bit register value.  Plain 16-bit
 * stores were observed to be dropped by the RIU on the real camera, so match
 * U-Boot here.
 */
static void sstar_udp_riu_write16(unsigned int off, u16 val)
{
	writel(val, sstar_udp_riu_va() + off);
}

static u32 sstar_udp_riu_read_reg(unsigned int g)
{
	return sstar_udp_riu_read16(2 * g) |
	       ((u32)sstar_udp_riu_read16(2 * g + 4) << 16);
}

static void sstar_udp_riu_write_reg(unsigned int g, u32 val)
{
	sstar_udp_riu_write16(2 * g, val & 0xffff);
	sstar_udp_riu_write16(2 * g + 4, val >> 16);	/* fires commands */
}

static void sstar_udp_kick(unsigned int len)
{
	unsigned int i;
	u32 tsr = 0;

	for (i = 0; i < SSTAR_POLL_LIMIT; i++) {
		tsr = sstar_udp_emac_read(SSTAR_EMAC_TSR);
		if ((tsr & SSTAR_EMAC_TSR_IDLE) == SSTAR_EMAC_TSR_IDLE)
			break;
		cpu_relax();
	}
	sstar_udp_bb_put(SSTAR_BB_TSR_BEFORE_W, tsr);

	sstar_udp_riu_write_reg(0x0c,
		(u32)(sstar_udp_frame_phys - SSTAR_DMA_OFFSET));	/* TAR */
	sstar_udp_riu_write_reg(0x10, len);			/* TCR */
	sstar_udp_bb_put(SSTAR_BB_X32_TAR_POST_W,
			 sstar_udp_emac_read(SSTAR_EMAC_TAR));

	/* Sample the MAC afterwards; a completed TX clears the busy bits. */
	wmb();
	sstar_udp_bb_put(SSTAR_BB_TSR_AFTER_W,
			 sstar_udp_emac_read(SSTAR_EMAC_TSR));
	sstar_udp_bb_put(SSTAR_BB_LAST_LEN_W, len);
}

static void sstar_udp_send(const char *payload, unsigned int len)
{
	unsigned int frame_len = SSTAR_UDP_HDR_LEN + len;
	unsigned int pad = 0;
	u8 hdr[SSTAR_UDP_HDR_LEN];
	void __iomem *frame;
	bool iomapped = sstar_udp_iomapped;

	if (!iomapped)
		preempt_disable();

	if (frame_len < SSTAR_UDP_MIN_FRAME) {
		pad = SSTAR_UDP_MIN_FRAME - frame_len;
		frame_len = SSTAR_UDP_MIN_FRAME;
	}

	sstar_udp_build_hdr(&sstar_udp_cfg, hdr, payload, len,
			    ++sstar_udp_ip_id);

	frame = sstar_udp_frame_va();
	memcpy_toio(frame, hdr, SSTAR_UDP_HDR_LEN);
	memcpy_toio(frame + SSTAR_UDP_HDR_LEN, payload, len);
	if (pad)
		memset_io(frame + SSTAR_UDP_HDR_LEN + len, 0, pad);

#ifdef CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_BLACKBOX
	sstar_udp_bb_put(SSTAR_BB_FRAME_W0_W, readl(frame));
	sstar_udp_bb_put(SSTAR_BB_FRAME_W1_W, readl(frame + 4));
	{
		static u32 bb_frames;
		sstar_udp_bb_put(SSTAR_BB_FRAMES_W, ++bb_frames);
	}
#endif

	/* The frame stores must reach MIU before the MAC reads it. */
	wmb();
	sstar_udp_flush_l3();
	sstar_udp_kick(frame_len);

	if (!iomapped)
		preempt_enable();
}

static void sstar_udp_console_write(struct console *con, const char *s,
				    unsigned int count)
{
	while (count) {
		unsigned int chunk = min_t(unsigned int, count,
					   SSTAR_UDP_MAX_PAYLOAD);

		sstar_udp_send(s, chunk);
		s += chunk;
		count -= chunk;
	}
}

static int __init sstar_udp_parse_mac(const char *s, u8 mac[6])
{
	unsigned int i;

	for (i = 0; i < 6; i++) {
		int hi, lo;

		hi = hex_to_bin(*s++);
		if (hi < 0)
			return -EINVAL;
		lo = hex_to_bin(*s++);
		if (lo < 0)
			return -EINVAL;
		mac[i] = (hi << 4) | lo;
		if (i < 5 && *s++ != ':')
			return -EINVAL;
	}

	return *s ? -EINVAL : 0;
}

static int __init sstar_udp_parse_ipv4(const char *s, u8 ip[4])
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		unsigned int val = 0;
		unsigned int digits = 0;

		while (*s >= '0' && *s <= '9') {
			val = val * 10 + (*s++ - '0');
			if (val > 255)
				return -EINVAL;
			digits++;
		}
		if (!digits)
			return -EINVAL;
		ip[i] = val;
		if (i < 3 && *s++ != '.')
			return -EINVAL;
	}

	return *s ? -EINVAL : 0;
}

/*
 * Take over from the fixmap with permanent uncached mappings.  ioremap()
 * needs the page allocator and the vmalloc area, so this can only succeed
 * once mm_core_init() has run; that is why an earlycon setup() cannot use
 * it (it runs from parse_early_param() inside setup_arch(), before
 * paging_init()) and why the fixmap path exists.  If the frame page is not
 * reserved no-map in the DT, ioremap() refuses to map RAM and we keep the
 * fixmap path, which works forever.
 */
/*
 * Optional destination overrides on the command line, for environments where
 * the boot host is not the address compiled into the defaults (e.g. a second
 * LAN box capturing the console because the harness host's firewall drops the
 * port).  The earlycon core passes the text after the address, so:
 *
 *   earlycon=sstar_udp,mmio,0x1f343c00,dip=172.17.1.181
 *   earlycon=sstar_udp,mmio,0x1f343c00,dip=172.17.1.181,dport=6666,mac=02:00:00:c2:12:02
 *
 * Unknown tokens (e.g. a bare baud rate) are ignored.
 */
static int __init sstar_udp_parse_opts(const char *opts)
{
	char buf[160];
	char *p, *tok;
	u8 tmp[6];

	strscpy(buf, opts, sizeof(buf));

	for (p = buf; (tok = strsep(&p, ",")) != NULL;) {
		if (!*tok)
			continue;
		if (!strncmp(tok, "dip=", 4)) {
			if (sstar_udp_parse_ipv4(tok + 4, sstar_udp_cfg.dst_ip))
				return -EINVAL;
		} else if (!strncmp(tok, "sip=", 4)) {
			if (sstar_udp_parse_ipv4(tok + 4, sstar_udp_cfg.src_ip))
				return -EINVAL;
		} else if (!strncmp(tok, "mac=", 4)) {
			if (sstar_udp_parse_mac(tok + 4, tmp))
				return -EINVAL;
			memcpy(sstar_udp_cfg.src_mac, tmp, SSTAR_UDP_MAC_LEN);
		} else if (!strncmp(tok, "dport=", 6)) {
			u16 v;

			if (kstrtou16(tok + 6, 10, &v))
				return -EINVAL;
			sstar_udp_cfg.dst_port = v;
		} else if (!strncmp(tok, "sport=", 6)) {
			u16 v;

			if (kstrtou16(tok + 6, 10, &v))
				return -EINVAL;
			sstar_udp_cfg.src_port = v;
		}
	}

	return 0;
}

static bool __init sstar_udp_try_ioremap(void)
{
	void __iomem *frame, *l3, *emac, *riu;

	frame = ioremap(sstar_udp_frame_phys, PAGE_SIZE);
	l3 = ioremap(CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_L3BRIDGE_BASE, PAGE_SIZE);
	emac = ioremap(sstar_udp_emac_phys & PAGE_MASK, PAGE_SIZE);
	riu = ioremap(SSTAR_EMAC_RIU_BASE, PAGE_SIZE);

	if (!frame || !l3 || !emac || !riu) {
		if (frame)
			iounmap(frame);
		if (l3)
			iounmap(l3);
		if (emac)
			iounmap(emac);
		if (riu)
			iounmap(riu);
		return false;
	}

	sstar_udp_frame_mapped = frame;
	sstar_udp_l3_mapped = l3;
	sstar_udp_emac_mapped = emac + sstar_udp_emac_off;
	sstar_udp_riu_mapped = riu;
	sstar_udp_iomapped = true;

	return true;
}

static int __init sstar_udp_setup(struct earlycon_device *dev, const char *opt)
{
	unsigned long base = sstar_udp_emac_phys;
	u8 mac[6], ip[4];

	/*
	 * The earlycon core parses "earlycon=sstar_udp,0x1f343c00" itself and
	 * passes the address in port.mapbase (options are then NULL); accept
	 * a leading hex option as well for callers that hand it to us, and
	 * ignore anything else (e.g. a baud string).
	 */
	if (dev->port.mapbase)
		base = dev->port.mapbase;
	else if (opt && !strncmp(opt, "0x", 2)) {
		unsigned long long val;

		if (!kstrtoull(opt, 0, &val))
			base = val;
	}

	if (!base || (base & 3))
		return -EINVAL;

	if (sstar_udp_parse_mac(CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_SRC_MAC, mac))
		return -EINVAL;
	if (sstar_udp_parse_ipv4(CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_SRC_IP, ip))
		return -EINVAL;
	memcpy(sstar_udp_cfg.src_mac, mac, SSTAR_UDP_MAC_LEN);
	memcpy(sstar_udp_cfg.src_ip, ip, SSTAR_UDP_IP_LEN);
	if (sstar_udp_parse_ipv4(CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_DST_IP, ip))
		return -EINVAL;
	memcpy(sstar_udp_cfg.dst_ip, ip, SSTAR_UDP_IP_LEN);
	sstar_udp_cfg.src_port = CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_SRC_PORT;
	sstar_udp_cfg.dst_port = CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_DST_PORT;

	/* Optional "dip=", "sip=", "mac=", "dport=", "sport=" overrides. */
	if (opt && strchr(opt, '=')) {
		if (sstar_udp_parse_opts(opt))
			return -EINVAL;
	}

	sstar_udp_emac_phys = base;
	sstar_udp_emac_off = base & ~PAGE_MASK;

	/*
	 * Normally not possible yet (the mm is not up inside setup_arch), but
	 * use ioremap() from the first frame if it ever is.
	 */
	if (slab_is_available())
		sstar_udp_try_ioremap();

	/*
	 * Keep the console for the whole boot: the retail board's only real
	 * console (ttyS0) has no pads, and a boot console would be
	 * unregistered the moment it becomes the default console.  See
	 * SERIAL_EARLYCON_SSSTAR_UDP_PERSISTENT.
	 */
	if (IS_ENABLED(CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_PERSISTENT))
		dev->con->flags &= ~CON_BOOT;

	sstar_udp_bb_map();
	sstar_udp_bb_put(SSTAR_BB_EMAC_PHYS_W, (u32)sstar_udp_emac_phys);
	sstar_udp_bb_put(SSTAR_BB_EMAC_OFF_W, (u32)sstar_udp_emac_off);
	sstar_udp_bb_put(SSTAR_BB_IOMAPPED_W, sstar_udp_iomapped ? 1 : 0);
	sstar_udp_bb_put(SSTAR_BB_CTL_PRE_W, sstar_udp_riu_read_reg(0x00));
	sstar_udp_bb_put(SSTAR_BB_CFG_W, sstar_udp_riu_read_reg(0x04));
	sstar_udp_bb_put(SSTAR_BB_SR_W, sstar_udp_riu_read_reg(0x08));
	sstar_udp_bb_put(SSTAR_BB_TSR_PRE_W, sstar_udp_riu_read_reg(0x14));
	sstar_udp_bb_put(SSTAR_BB_ISR_W, sstar_udp_riu_read_reg(0x24));
	sstar_udp_bb_put(SSTAR_BB_IMR_W, sstar_udp_riu_read_reg(0x30));

	/*
	 * Configure the MAC through the X32 view (the view the vendor 4.9
	 * driver uses for these registers, and the one whose writes land on
	 * the real camera): mask all interrupts, then enable RX+TX like the
	 * vendor MDev_EMAC_start(), keeping the management-port bit U-Boot
	 * leaves set, and set JULIAN 0x104 SOFTWARE_DESCRIPTOR_ENABLE - the
	 * bit that makes TAR/TCR a single-packet transmit.  It is set on the
	 * live vendor 4.9 system but clear in the state U-Boot leaves behind.
	 */
	/* Write CTL through the RIU view, the one U-Boot itself uses. */
	sstar_udp_riu_write_reg(0x2c, 0xffffffff);		/* IDR */
	sstar_udp_riu_write_reg(0x00,
				(sstar_udp_riu_read_reg(0x00) & 0xffff) |
				0x1c);				/* MPE|RE|TE */
	/* Cross-check through the other view: did the RIU write land? */
	sstar_udp_bb_put(SSTAR_BB_CTL_POST_W,
			 sstar_udp_emac_read(SSTAR_EMAC_CTL));
	sstar_udp_bb_put(SSTAR_BB_CTL_RIU_POST_W,
			 sstar_udp_riu_read_reg(0x00));
	sstar_udp_bb_put(SSTAR_BB_CTL_READW_W,
			 readw(sstar_udp_riu_va() + 2 * 0x00));
	sstar_udp_bb_put(SSTAR_BB_IDR_W, sstar_udp_riu_read_reg(0x2c));


	/* The internal "Albany" PHY: register N is the 16-bit RIU word at +4N. */
	sstar_udp_bb_put(SSTAR_BB_PHY_BMCR_W,
			 readw(sstar_udp_phy_va() + 0 * 4));
	sstar_udp_bb_put(SSTAR_BB_PHY_BMSR_W,
			 readw(sstar_udp_phy_va() + 1 * 4));

	dev->con->write = sstar_udp_console_write;
	sstar_udp_enabled = true;

	return 0;
}
EARLYCON_DECLARE(sstar_udp, sstar_udp_setup);

/*
 * MAC loopback self-test: put the MAC in loopback, hand it one perfectly
 * formed frame with the normal TAR/TCR transmit path and see whether the
 * receive side stores it back to memory.  This separates "the MAC transmits
 * and DMAs both ways" from "the frames leave the MAC but never reach the
 * wire/PHY", which is otherwise unobservable without a UART.
 */
#ifdef CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_DEBUG

#define SSTAR_RX_TEST_PHYS	CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_RXTEST_PHYS

static void __init sstar_udp_loopback_test(void)
{
	void __iomem *riu, *rx, *emac;
	u32 ring_bus, buf_bus, desc0, desc1;
	unsigned int i;
	static const char payload[] = "C212-LOOPBACK-SELFTEST";

	if (!sstar_udp_enabled)
		return;

	rx = ioremap(SSTAR_RX_TEST_PHYS, PAGE_SIZE);
	if (!rx)
		return;
	memset_io(rx, 0, PAGE_SIZE);

	ring_bus = (u32)(SSTAR_RX_TEST_PHYS - SSTAR_DMA_OFFSET);
	buf_bus = ring_bus + 0x800;

	/* One-entry RX ring: DONE|WRAP, buffer address in bits 31:2. */
	writel(buf_bus | 0x3, rx);
	writel(0, rx + 4);

	riu = sstar_udp_riu_va();
	/* RBQP = ring, then enable RX + loopback + TX. */
	sstar_udp_riu_write_reg(0x18, ring_bus);
	sstar_udp_riu_write_reg(0x00,
				(sstar_udp_riu_read_reg(0x00) & 0xffff) |
				0x1d);			/* MPE|RE|TE|LB */

	/* Send one frame through the normal path. */
	sstar_udp_send(payload, sizeof(payload) - 1);

	for (i = 0; i < 100000; i++) {
		if (!(readl(rx) & 1))		/* DONE cleared = received */
			break;
		cpu_relax();
	}

	desc0 = readl(rx);
	desc1 = readl(rx + 4);
	sstar_udp_bb_put(SSTAR_BB_LOOP_DESC0_W, desc0);
	sstar_udp_bb_put(SSTAR_BB_LOOP_DESC1_W, desc1);
	sstar_udp_bb_put(SSTAR_BB_LOOP_RXBUF0_W, readl(rx + 0x800));
	sstar_udp_bb_put(SSTAR_BB_LOOP_RXBUF1_W, readl(rx + 0x804));
	sstar_udp_bb_put(SSTAR_BB_LOOP_RXBUF2_W, readl(rx + 0x808));
	sstar_udp_bb_put(SSTAR_BB_LOOP_RSR_W, sstar_udp_riu_read_reg(0x20));
	sstar_udp_bb_put(SSTAR_BB_LOOP_CTL_W, sstar_udp_riu_read_reg(0x00));

	/* Leave loopback, restore RX off (the boot never needs RX). */
	sstar_udp_riu_write_reg(0x00,
				(sstar_udp_riu_read_reg(0x00) & 0xffff) &
				~(0x1 | 0x4));		/* clear LB|RE */
	emac = sstar_udp_emac_va();
	writel(0, emac + SSTAR_EMAC_RSR_ADDR);	/* clear RX status */
	iounmap(rx);
}

/*
 * Crash-box register dump: X32 view (0x1F343C00..0x1F343E00, as 32-bit words)
 * followed by the EMAC RIU view (0x1F2A2000..0x1F2A2400, as 16-bit halves)
 * into a reserved page, so the post-fallback OpenIPC boot can diff the state
 * mainline inherits from U-Boot against the live vendor driver's state.
 */
#define SSTAR_DUMP_PHYS	CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_DUMP_PHYS

static void __init sstar_udp_reg_dump(void)
{
	void __iomem *out, *emac, *riu;
	unsigned int i;

	if (!sstar_udp_enabled || !sstar_udp_iomapped)
		return;

	out = ioremap(SSTAR_DUMP_PHYS, PAGE_SIZE);
	if (!out)
		return;
	memset_io(out, 0, PAGE_SIZE);

	emac = sstar_udp_emac_va();
	for (i = 0; i < 0x200; i += 4)
		writel_relaxed(readl(emac + i), out + i);

	riu = sstar_udp_riu_va();
	for (i = 0; i < 0x400; i += 2)
		writew_relaxed(readw(riu + i), out + 0x200 + i);

	/* The internal Albany PHY window: banks 0x31..0x33 at 0x1F006200. */
	riu = sstar_udp_phy_va();
	for (i = 0; i < 0x200; i += 2)
		writew_relaxed(readw(riu + i), out + 0x600 + i);

	iounmap(out);
}

#else /* !CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_DEBUG */

static void __init sstar_udp_loopback_test(void) { }
static void __init sstar_udp_reg_dump(void) { }

#endif /* CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_DEBUG */

/* The mm is up once the early initcalls run: drop the fixmap remapping. */
static int __init sstar_udp_remap(void)
{
	if (!sstar_udp_enabled || sstar_udp_iomapped)
		return 0;

	if (sstar_udp_try_ioremap())
		pr_info("sstar_udp: switched to ioremap packet buffer\n");
	else
		pr_warn("sstar_udp: ioremap failed, keeping fixmap mappings\n");

	/* Install the black box page and replay the pre-mm samples. */
	sstar_udp_bb_map();

	/* Record what the MAC/PHY look like once the mm is up, for the black box. */
	{
		void __iomem *emac = sstar_udp_emac_va();

		sstar_udp_bb_put(SSTAR_BB_REMAP_DONE_W, sstar_udp_iomapped ? 1 : 0);
		sstar_udp_bb_put(SSTAR_BB_CTL_LATE_W,
				 sstar_udp_riu_read_reg(0x00));
		sstar_udp_bb_put(SSTAR_BB_TSR_LATE_W,
				 sstar_udp_riu_read_reg(0x14));
		sstar_udp_bb_put(SSTAR_BB_SR_LATE_W,
				 sstar_udp_riu_read_reg(0x08));
		sstar_udp_bb_put(SSTAR_BB_IMR_LATE_W,
				 sstar_udp_riu_read_reg(0x30));
		/* Keep the X32 read-back as a cross-check for the black box. */
		sstar_udp_bb_put(SSTAR_BB_X32_CTL_LATE_W,
				 readl(emac + SSTAR_EMAC_CTL));
		sstar_udp_bb_put(SSTAR_BB_PHY_BMSR_LATE_W,
				 readw(sstar_udp_phy_va() + 1 * 4));
		sstar_udp_bb_put(SSTAR_BB_TXQ_USED_LATE_W,
				 readb(sstar_udp_riu_va() +
				       SSTAR_EMAC1_TXQ_USED) & 0x7f);
		/* One more enable in case something cleared it during boot. */
		sstar_udp_emac_txq_ping();
	}

	sstar_udp_loopback_test();
	sstar_udp_reg_dump();

	return 0;
}
early_initcall(sstar_udp_remap);
