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
#define SSTAR_EMAC_TAR			0x18
#define SSTAR_EMAC_TCR			0x20
#define SSTAR_EMAC_TSR			0x28
#define SSTAR_EMAC_IDR			0x58

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

	writel_relaxed(SSTAR_L3BRIDGE_FLUSH_1, l3 + SSTAR_L3BRIDGE_FLUSH);
	writel_relaxed(SSTAR_L3BRIDGE_FLUSH_2, l3 + SSTAR_L3BRIDGE_FLUSH);

	for (i = 0; i < SSTAR_POLL_LIMIT; i++) {
		if (readl_relaxed(l3 + SSTAR_L3BRIDGE_STATUS) &
		    SSTAR_L3BRIDGE_DONE)
			return;
		cpu_relax();
	}
}

/* Hand one complete frame to the MAC's single-packet TX slot. */
static void sstar_udp_kick(unsigned int len)
{
	void __iomem *emac = sstar_udp_emac_va();
	unsigned int i;

	for (i = 0; i < SSTAR_POLL_LIMIT; i++) {
		if ((readl(emac + SSTAR_EMAC_TSR) & SSTAR_EMAC_TSR_IDLE) ==
		    SSTAR_EMAC_TSR_IDLE)
			break;
		cpu_relax();
	}

	writel((u32)(sstar_udp_frame_phys - SSTAR_DMA_OFFSET),
	       emac + SSTAR_EMAC_TAR);
	writel(len, emac + SSTAR_EMAC_TCR);
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
static bool __init sstar_udp_try_ioremap(void)
{
	void __iomem *frame, *l3, *emac;

	frame = ioremap(sstar_udp_frame_phys, PAGE_SIZE);
	l3 = ioremap(CONFIG_SERIAL_EARLYCON_SSSTAR_UDP_L3BRIDGE_BASE, PAGE_SIZE);
	emac = ioremap(sstar_udp_emac_phys & PAGE_MASK, PAGE_SIZE);

	if (!frame || !l3 || !emac) {
		if (frame)
			iounmap(frame);
		if (l3)
			iounmap(l3);
		if (emac)
			iounmap(emac);
		return false;
	}

	sstar_udp_frame_mapped = frame;
	sstar_udp_l3_mapped = l3;
	sstar_udp_emac_mapped = emac + sstar_udp_emac_off;
	sstar_udp_iomapped = true;

	return true;
}

static int __init sstar_udp_setup(struct earlycon_device *dev, const char *opt)
{
	void __iomem *emac;
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

	sstar_udp_emac_phys = base;
	sstar_udp_emac_off = base & ~PAGE_MASK;

	/*
	 * Normally not possible yet (the mm is not up inside setup_arch), but
	 * use ioremap() from the first frame if it ever is.
	 */
	if (slab_is_available())
		sstar_udp_try_ioremap();

	/* Mask MAC interrupts and re-enable only the transmitter. */
	emac = sstar_udp_emac_va();
	writel(0xffffffff, emac + SSTAR_EMAC_IDR);
	writel(readl(emac + SSTAR_EMAC_CTL) | SSTAR_EMAC_CTL_TE,
	       emac + SSTAR_EMAC_CTL);

	dev->con->write = sstar_udp_console_write;
	sstar_udp_enabled = true;

	return 0;
}
EARLYCON_DECLARE(sstar_udp, sstar_udp_setup);

/* The mm is up once the early initcalls run: drop the fixmap remapping. */
static int __init sstar_udp_remap(void)
{
	if (!sstar_udp_enabled || sstar_udp_iomapped)
		return 0;

	if (sstar_udp_try_ioremap())
		pr_info("sstar_udp: switched to ioremap packet buffer\n");
	else
		pr_warn("sstar_udp: ioremap failed, keeping fixmap mappings\n");

	return 0;
}
early_initcall(sstar_udp_remap);
