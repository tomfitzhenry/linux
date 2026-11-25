// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KUnit tests for NetLabel IPv6 CALIPSO (CALIPSO_MAP_PASS & CALIPSO_MAP_LOCAL).
 *
 * Included by net/ipv6/calipso.c when CONFIG_NETLABEL_CALIPSO_KUNIT_TEST=y.
 */

#include <kunit/test.h>
#include <linux/netdevice.h>

#define CALIPSO_TEST_DOI_LOCAL 0x4c4f4301U
#define CALIPSO_TEST_DOI_PASS  0x4c4f4302U

static void calipso_test_recompute_crc(unsigned char *opt)
{
	u16 crc, len = opt[1] + 2;

	opt[8] = 0;
	opt[9] = 0;
	crc = ~crc_ccitt(0xffff, opt, len);
	opt[8] = crc & 0xff;
	opt[9] = (crc >> 8) & 0xff;
}

static void calipso_test_doi_cleanup(void *data)
{
	u32 doi = (u32)(uintptr_t)data;
	struct netlbl_audit audit_info = {};

	calipso_doi_remove(doi, &audit_info);
}

static struct calipso_doi *calipso_test_add_doi(struct kunit *test,
						u32 doi, u32 type)
{
	struct netlbl_audit audit_info = {};
	struct calipso_doi *doi_def;
	int ret;

	doi_def = kmalloc_obj(*doi_def);
	KUNIT_ASSERT_NOT_NULL(test, doi_def);
	doi_def->doi = doi;
	doi_def->type = type;

	ret = calipso_doi_add(doi_def, &audit_info);
	if (ret != 0) {
		calipso_doi_free(doi_def);
		KUNIT_FAIL(test, "calipso_doi_add(%u, %u) failed: %d",
			   doi, type, ret);
		return NULL;
	}

	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  calipso_test_doi_cleanup,
						  (void *)(uintptr_t)doi),
			0);
	return doi_def;
}

static void calipso_test_doi_add_invalid(struct kunit *test)
{
	struct netlbl_audit audit_info = {};
	struct calipso_doi doi_def = {
		.doi = CALIPSO_DOI_UNKNOWN,
		.type = CALIPSO_MAP_LOCAL,
	};

	KUNIT_EXPECT_EQ(test, calipso_doi_add(&doi_def, &audit_info), -EINVAL);

	doi_def.doi = CALIPSO_TEST_DOI_LOCAL;
	doi_def.type = CALIPSO_MAP_UNKNOWN;
	KUNIT_EXPECT_EQ(test, calipso_doi_add(&doi_def, &audit_info), -EINVAL);

	doi_def.type = 99;
	KUNIT_EXPECT_EQ(test, calipso_doi_add(&doi_def, &audit_info), -EINVAL);
}

static void calipso_test_local_genopt_and_getattr(struct kunit *test)
{
	struct calipso_doi *doi_def;
	struct netlbl_lsm_secattr in_attr, out_attr;
	unsigned char buf[32];
	unsigned char *opt;
	int len;

	doi_def = calipso_test_add_doi(test, CALIPSO_TEST_DOI_LOCAL,
				       CALIPSO_MAP_LOCAL);

	/* Missing NETLBL_SECATTR_SECID must fail with -EPERM. */
	netlbl_secattr_init(&in_attr);
	in_attr.flags = NETLBL_SECATTR_MLS_LVL;
	in_attr.attr.mls.lvl = 1;
	KUNIT_EXPECT_EQ(test,
			calipso_genopt(buf, 2, sizeof(buf), doi_def, &in_attr),
			-EPERM);

	/* Buffer too small must fail with -ENOSPC. */
	netlbl_secattr_init(&in_attr);
	in_attr.flags = NETLBL_SECATTR_SECID;
	in_attr.attr.secid = 0x12345678U;
	KUNIT_EXPECT_EQ(test,
			calipso_genopt(buf, 2, 2 + CALIPSO_LOC_OPT_LEN - 1,
				       doi_def, &in_attr),
			-ENOSPC);

	/* Valid local option at 4n + 2 alignment (start = 2 => pad = 0). */
	memset(buf, 0xaa, sizeof(buf));
	len = calipso_genopt(buf, 2, sizeof(buf), doi_def, &in_attr);
	KUNIT_ASSERT_EQ(test, len, (int)CALIPSO_LOC_OPT_LEN);

	opt = buf + 2;
	KUNIT_EXPECT_EQ(test, opt[0], (unsigned char)IPV6_TLV_CALIPSO);
	KUNIT_EXPECT_EQ(test, opt[1], (unsigned char)(CALIPSO_LOC_OPT_LEN - 2));
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(opt + 2),
			CALIPSO_TEST_DOI_LOCAL);
	KUNIT_EXPECT_EQ(test, opt[6], (unsigned char)1);
	KUNIT_EXPECT_EQ(test, opt[7], (unsigned char)0);

	netlbl_secattr_init(&out_attr);
	KUNIT_ASSERT_EQ(test, calipso_opt_getattr(opt, &out_attr), 0);
	KUNIT_EXPECT_EQ(test, out_attr.type, (u32)NETLBL_NLTYPE_CALIPSO);
	KUNIT_EXPECT_TRUE(test, !!(out_attr.flags & NETLBL_SECATTR_SECID));
	KUNIT_EXPECT_FALSE(test, !!(out_attr.flags & NETLBL_SECATTR_MLS_LVL));
	KUNIT_EXPECT_EQ(test, out_attr.attr.secid, 0x12345678U);
	netlbl_secattr_destroy(&out_attr);
}

static struct net_device *calipso_test_alloc_dev(struct kunit *test,
						 unsigned int flags)
{
	struct net_device *dev;

	dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	dev->flags = flags;
	return dev;
}

static void calipso_test_local_validate_loopback(struct kunit *test)
{
	struct calipso_doi *doi_def;
	struct netlbl_lsm_secattr attr;
	struct net_device *lo_dev = calipso_test_alloc_dev(test, IFF_LOOPBACK);
	struct net_device *eth_dev = calipso_test_alloc_dev(test, 0);
	struct sk_buff skb = {};
	unsigned char buf[32];
	unsigned char bad_opt[32];
	unsigned char *opt;
	int len;

	doi_def = calipso_test_add_doi(test, CALIPSO_TEST_DOI_LOCAL,
				       CALIPSO_MAP_LOCAL);

	netlbl_secattr_init(&attr);
	attr.flags = NETLBL_SECATTR_SECID;
	attr.attr.secid = 0xabcdef01U;

	memset(buf, 0, sizeof(buf));
	len = calipso_genopt(buf, 2, sizeof(buf), doi_def, &attr);
	KUNIT_ASSERT_EQ(test, len, (int)CALIPSO_LOC_OPT_LEN);
	opt = buf + 2;

	/* Accepted on loopback interface. */
	skb.dev = lo_dev;
	KUNIT_EXPECT_TRUE(test, calipso_validate(&skb, opt));

	/* Rejected on non-loopback interface, NULL skb, or NULL skb->dev. */
	skb.dev = eth_dev;
	KUNIT_EXPECT_FALSE(test, calipso_validate(&skb, opt));
	KUNIT_EXPECT_FALSE(test, calipso_validate(NULL, opt));
	skb.dev = NULL;
	KUNIT_EXPECT_FALSE(test, calipso_validate(&skb, opt));

	/* Rejected when CRC is invalid. */
	skb.dev = lo_dev;
	memcpy(bad_opt, opt, CALIPSO_LOC_OPT_LEN);
	bad_opt[10] ^= 0x01;
	KUNIT_EXPECT_FALSE(test, calipso_validate(&skb, bad_opt));

	/* Rejected when sensitivity level is non-zero even with valid CRC. */
	memcpy(bad_opt, opt, CALIPSO_LOC_OPT_LEN);
	bad_opt[7] = 1;
	calipso_test_recompute_crc(bad_opt);
	KUNIT_EXPECT_FALSE(test, calipso_validate(&skb, bad_opt));

	/* Rejected when option length is wrong (10 bytes, cmpt_len = 0). */
	memcpy(bad_opt, opt, CALIPSO_HDR_LEN);
	bad_opt[1] = CALIPSO_HDR_LEN - 2;
	bad_opt[6] = 0;
	calipso_test_recompute_crc(bad_opt);
	KUNIT_EXPECT_FALSE(test, calipso_validate(&skb, bad_opt));
}

static void calipso_test_pass_no_categories(struct kunit *test)
{
	struct calipso_doi *doi_def;
	struct netlbl_lsm_secattr in_attr, out_attr;
	struct net_device *eth_dev = calipso_test_alloc_dev(test, 0);
	struct sk_buff skb = { .dev = eth_dev };
	unsigned char buf[32];
	unsigned char *opt;
	int len;

	doi_def = calipso_test_add_doi(test, CALIPSO_TEST_DOI_PASS,
				       CALIPSO_MAP_PASS);

	netlbl_secattr_init(&in_attr);
	in_attr.flags = NETLBL_SECATTR_MLS_LVL;
	in_attr.attr.mls.lvl = 5;

	memset(buf, 0, sizeof(buf));
	len = calipso_genopt(buf, 2, sizeof(buf), doi_def, &in_attr);
	KUNIT_ASSERT_EQ(test, len, (int)CALIPSO_HDR_LEN);
	opt = buf + 2;

	/* Pass-through DOI is allowed on non-loopback devices. */
	KUNIT_EXPECT_TRUE(test, calipso_validate(&skb, opt));

	netlbl_secattr_init(&out_attr);
	KUNIT_ASSERT_EQ(test, calipso_opt_getattr(opt, &out_attr), 0);
	KUNIT_EXPECT_EQ(test, out_attr.type, (u32)NETLBL_NLTYPE_CALIPSO);
	KUNIT_EXPECT_TRUE(test, !!(out_attr.flags & NETLBL_SECATTR_MLS_LVL));
	KUNIT_EXPECT_FALSE(test, !!(out_attr.flags & NETLBL_SECATTR_SECID));
	KUNIT_EXPECT_EQ(test, out_attr.attr.mls.lvl, (u32)5);
	netlbl_secattr_destroy(&out_attr);
}

static void calipso_test_local_skbuff_roundtrip(struct kunit *test)
{
	struct calipso_doi *doi_def;
	struct netlbl_lsm_secattr in_attr, out_attr;
	struct net_device *lo_dev = calipso_test_alloc_dev(test, IFF_LOOPBACK);
	struct net_device *eth_dev = calipso_test_alloc_dev(test, 0);
	struct sk_buff *skb;
	struct ipv6hdr *ip6h;
	struct ipv6_opt_hdr *hop;
	unsigned char *opt;

	doi_def = calipso_test_add_doi(test, CALIPSO_TEST_DOI_LOCAL,
				       CALIPSO_MAP_LOCAL);

	skb = alloc_skb(128, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_reserve(skb, 64);
	ip6h = skb_put_zero(skb, sizeof(*ip6h) + 20);
	skb_reset_network_header(skb);
	ip6h->version = 6;
	ip6h->nexthdr = NEXTHDR_TCP;
	ip6h->payload_len = htons(20);

	netlbl_secattr_init(&in_attr);
	in_attr.flags = NETLBL_SECATTR_SECID;
	in_attr.attr.secid = 0xdeadbeefU;

	KUNIT_ASSERT_EQ(test, calipso_skbuff_setattr(skb, doi_def, &in_attr), 0);

	ip6h = ipv6_hdr(skb);
	KUNIT_EXPECT_EQ(test, ip6h->nexthdr, (u8)NEXTHDR_HOP);
	KUNIT_EXPECT_EQ(test, ntohs(ip6h->payload_len), (u16)(20 + 16));
	hop = (struct ipv6_opt_hdr *)(ip6h + 1);
	KUNIT_EXPECT_EQ(test, (int)ipv6_optlen(hop), 16);

	opt = calipso_skbuff_optptr(skb);
	KUNIT_ASSERT_NOT_NULL(test, opt);

	skb->dev = lo_dev;
	KUNIT_EXPECT_TRUE(test, calipso_validate(skb, opt));
	skb->dev = eth_dev;
	KUNIT_EXPECT_FALSE(test, calipso_validate(skb, opt));

	netlbl_secattr_init(&out_attr);
	KUNIT_ASSERT_EQ(test, calipso_opt_getattr(opt, &out_attr), 0);
	KUNIT_EXPECT_EQ(test, out_attr.attr.secid, 0xdeadbeefU);
	KUNIT_EXPECT_TRUE(test, !!(out_attr.flags & NETLBL_SECATTR_SECID));
	netlbl_secattr_destroy(&out_attr);

	KUNIT_ASSERT_EQ(test, calipso_skbuff_delattr(skb), 0);
	ip6h = ipv6_hdr(skb);
	KUNIT_EXPECT_EQ(test, ip6h->nexthdr, (u8)NEXTHDR_TCP);
	KUNIT_EXPECT_EQ(test, ntohs(ip6h->payload_len), (u16)20);
	KUNIT_EXPECT_NULL(test, calipso_skbuff_optptr(skb));

	kfree_skb(skb);
}

static struct kunit_case calipso_test_cases[] = {
	KUNIT_CASE(calipso_test_doi_add_invalid),
	KUNIT_CASE(calipso_test_local_genopt_and_getattr),
	KUNIT_CASE(calipso_test_local_validate_loopback),
	KUNIT_CASE(calipso_test_pass_no_categories),
	KUNIT_CASE(calipso_test_local_skbuff_roundtrip),
	{}
};

static struct kunit_suite calipso_test_suite = {
	.name = "calipso",
	.test_cases = calipso_test_cases,
};

kunit_test_suite(calipso_test_suite);
