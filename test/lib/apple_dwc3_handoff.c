// SPDX-License-Identifier: GPL-2.0+

#include <asm/byteorder.h>
#include <errno.h>
#include <linux/bitops.h>
#include <linux/sizes.h>
#include <test/lib.h>
#include <test/test.h>
#include <test/ut.h>
#include <usb/apple_dwc3_handoff.h>

#define TEST_DESCRIPTOR_PHYS	0x100000ULL
#define TEST_WDT_PHYS		0x3882b0000ULL
#define TEST_DWC3_PHYS		0x700100000ULL

static const struct apple_dwc3_handoff_region test_ram[] = {
	{ TEST_DESCRIPTOR_PHYS, 0x40000 },
};

static const struct apple_dwc3_handoff_region test_reserved[] = {
	{ TEST_DESCRIPTOR_PHYS, 0x40000 },
};

static const struct apple_dwc3_handoff_region test_mmio[] = {
	{ 0x700000000ULL, SZ_1G },
};

static const struct apple_dwc3_handoff_limits test_limits = {
	.ram = test_ram,
	.ram_count = ARRAY_SIZE(test_ram),
	.reserved = test_reserved,
	.reserved_count = ARRAY_SIZE(test_reserved),
	.mmio = test_mmio,
	.mmio_count = ARRAY_SIZE(test_mmio),
	.expected_wdt_regs = TEST_WDT_PHYS,
};

static void setup_valid_descriptor(struct apple_dwc3_handoff_raw *raw)
{
	memset(raw, 0, sizeof(*raw));
	raw->magic = cpu_to_le64(APPLE_DWC3_HANDOFF_MAGIC);
	raw->version = cpu_to_le32(APPLE_DWC3_HANDOFF_VERSION);
	raw->size = cpu_to_le32(sizeof(*raw));
	raw->flags = cpu_to_le64(APPLE_DWC3_HANDOFF_READY);
	raw->regs_phys = cpu_to_le64(TEST_DWC3_PHYS);
	raw->event_buffer_phys = cpu_to_le64(0x104000);
	raw->event_buffer_size = cpu_to_le32(SZ_16K);
	raw->event_buffer_offset = cpu_to_le32(7);
	raw->scratchpad_phys = cpu_to_le64(0x108000);
	raw->scratchpad_size = cpu_to_le64(SZ_16K);
	raw->xfer_buffer_phys = cpu_to_le64(0x10c000);
	raw->xfer_buffer_size = cpu_to_le64(2 * SZ_16K);
	raw->trb_buffer_phys = cpu_to_le64(0x114000);
	raw->trb_buffer_size = cpu_to_le64(SZ_16K);
	raw->tx_buffer_phys = cpu_to_le64(0x10c000);
	raw->tx_buffer_iova = cpu_to_le64(0x200000);
	raw->tx_trb_phys = cpu_to_le64(0x114000);
	raw->tx_trb_iova = cpu_to_le64(0x208000);
	raw->tx_endpoint = cpu_to_le32(APPLE_DWC3_HANDOFF_TX_ENDPOINT);
	raw->tx_max_packet = cpu_to_le32(SZ_16K);
	raw->tx_busy = cpu_to_le32(1);
	raw->stage = cpu_to_le32(5);
	raw->rx_buffer_phys = cpu_to_le64(0x110000);
	raw->rx_buffer_iova = cpu_to_le64(0x204000);
	raw->rx_trb_phys = cpu_to_le64(0x114010);
	raw->rx_trb_iova = cpu_to_le64(0x208010);
	raw->wdt_regs_phys = cpu_to_le64(TEST_WDT_PHYS);
	raw->rx_endpoint = cpu_to_le32(APPLE_DWC3_HANDOFF_RX_ENDPOINT);
	raw->rx_busy = cpu_to_le32(1);
}

static int test_validate(const struct apple_dwc3_handoff_raw *raw,
			 u64 descriptor_phys,
			 const struct apple_dwc3_handoff_limits *limits)
{
	struct apple_dwc3_handoff_desc desc;

	return apple_dwc3_handoff_validate(raw, descriptor_phys, limits, &desc);
}

static int lib_test_apple_dwc3_handoff_valid(struct unit_test_state *uts)
{
	struct apple_dwc3_handoff_desc desc;
	struct apple_dwc3_handoff_raw raw;

	setup_valid_descriptor(&raw);
	ut_assertok(apple_dwc3_handoff_validate(&raw, TEST_DESCRIPTOR_PHYS,
						&test_limits, &desc));
	ut_asserteq_64(TEST_DWC3_PHYS, desc.regs_phys);
	ut_asserteq(7, desc.event_buffer_offset);
	ut_asserteq(5, desc.stage);

	/* Unknown flags and a future descriptor tail are ignored. */
	raw.flags = cpu_to_le64(APPLE_DWC3_HANDOFF_READY | BIT_ULL(63));
	raw.size = cpu_to_le32(256);
	ut_assertok(apple_dwc3_handoff_validate(&raw, TEST_DESCRIPTOR_PHYS,
						&test_limits, &desc));

	return 0;
}

LIB_TEST(lib_test_apple_dwc3_handoff_valid, 0);

static int lib_test_apple_dwc3_handoff_header(struct unit_test_state *uts)
{
	struct apple_dwc3_handoff_raw raw;

	setup_valid_descriptor(&raw);
	raw.magic = 0;
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	setup_valid_descriptor(&raw);
	raw.version = cpu_to_le32(APPLE_DWC3_HANDOFF_VERSION + 1);
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	setup_valid_descriptor(&raw);
	raw.size = cpu_to_le32(sizeof(raw) - 1);
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	setup_valid_descriptor(&raw);
	raw.flags = 0;
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	setup_valid_descriptor(&raw);
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS + 8, &test_limits));

	return 0;
}

LIB_TEST(lib_test_apple_dwc3_handoff_header, 0);

static int lib_test_apple_dwc3_handoff_ranges(struct unit_test_state *uts)
{
	const struct apple_dwc3_handoff_region descriptor_only[] = {
		{ TEST_DESCRIPTOR_PHYS, SZ_16K },
	};
	struct apple_dwc3_handoff_limits limits = test_limits;
	struct apple_dwc3_handoff_raw raw;

	setup_valid_descriptor(&raw);
	raw.event_buffer_offset = cpu_to_le32(SZ_16K / sizeof(u32));
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	setup_valid_descriptor(&raw);
	raw.event_buffer_phys = cpu_to_le64(0x140000);
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	/* RAM alone is insufficient: every shared range must be reserved. */
	setup_valid_descriptor(&raw);
	limits.reserved = descriptor_only;
	limits.reserved_count = ARRAY_SIZE(descriptor_only);
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &limits));

	setup_valid_descriptor(&raw);
	raw.rx_buffer_phys = raw.tx_buffer_phys;
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	setup_valid_descriptor(&raw);
	raw.rx_trb_iova = cpu_to_le64(U64_MAX - 7);
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	setup_valid_descriptor(&raw);
	raw.scratchpad_phys = raw.event_buffer_phys;
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	return 0;
}

LIB_TEST(lib_test_apple_dwc3_handoff_ranges, 0);

static int lib_test_apple_dwc3_handoff_event_wrap(struct unit_test_state *uts)
{
	ut_asserteq(1, apple_dwc3_next_event(0, SZ_16K));
	ut_asserteq(0,
		    apple_dwc3_next_event(SZ_16K / sizeof(u32) - 1, SZ_16K));

	return 0;
}

LIB_TEST(lib_test_apple_dwc3_handoff_event_wrap, 0);

static int lib_test_apple_dwc3_handoff_platform(struct unit_test_state *uts)
{
	struct apple_dwc3_handoff_raw raw;

	setup_valid_descriptor(&raw);
	raw.regs_phys = cpu_to_le64(0x380000000ULL);
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	setup_valid_descriptor(&raw);
	raw.wdt_regs_phys = cpu_to_le64(TEST_WDT_PHYS + SZ_4K);
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	setup_valid_descriptor(&raw);
	raw.tx_endpoint = cpu_to_le32(7);
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	setup_valid_descriptor(&raw);
	raw.tx_max_packet = cpu_to_le32(508);
	ut_asserteq(-EINVAL,
		    test_validate(&raw, TEST_DESCRIPTOR_PHYS, &test_limits));

	return 0;
}

LIB_TEST(lib_test_apple_dwc3_handoff_platform, 0);
