// SPDX-License-Identifier: GPL-2.0+

#include <apple_mtp.h>
#include <test/lib.h>
#include <test/test.h>
#include <test/ut.h>

struct apple_mtp_test_frame {
	struct apple_mtp_header hdr;
	struct apple_mtp_subheader sub;
	u8 command[4];
	__le32 checksum;
} __packed;

static void apple_mtp_test_make_frame(struct apple_mtp_test_frame *frame)
{
	u32 checksum;

	memset(frame, 0, sizeof(*frame));
	frame->hdr.hdr_len = sizeof(frame->hdr);
	frame->hdr.channel = APPLE_MTP_CHANNEL_COMMAND;
	frame->hdr.length = cpu_to_le16(sizeof(frame->sub) +
					    sizeof(frame->command));
	frame->hdr.seq = 3;
	frame->hdr.iface = 2;
	frame->sub.flags = 0x80;
	frame->sub.length = cpu_to_le16(2);
	frame->command[0] = 0xb4;
	frame->command[1] = 2;
	checksum = ~0U -
		apple_mtp_checksum(frame, sizeof(*frame) - sizeof(frame->checksum));
	frame->checksum = cpu_to_le32(checksum);
}

static int lib_test_apple_mtp_frame(struct unit_test_state *uts)
{
	struct apple_mtp_test_frame frame;

	apple_mtp_test_make_frame(&frame);
	ut_assertok(apple_mtp_validate_frame(&frame, sizeof(frame)));
	ut_asserteq(~0U, apple_mtp_checksum(&frame, sizeof(frame)));

	frame.command[1] ^= 1;
	ut_asserteq(-EBADMSG,
		    apple_mtp_validate_frame(&frame, sizeof(frame)));

	apple_mtp_test_make_frame(&frame);
	frame.hdr.hdr_len--;
	ut_asserteq(-EPROTO,
		    apple_mtp_validate_frame(&frame, sizeof(frame)));

	apple_mtp_test_make_frame(&frame);
	frame.hdr.length = cpu_to_le16(le16_to_cpu(frame.hdr.length) - 1);
	ut_asserteq(-EMSGSIZE,
		    apple_mtp_validate_frame(&frame, sizeof(frame)));

	apple_mtp_test_make_frame(&frame);
	ut_asserteq(-EMSGSIZE,
		    apple_mtp_validate_frame(&frame, sizeof(frame) - 1));
	ut_asserteq(-EMSGSIZE,
		    apple_mtp_validate_frame(&frame, sizeof(frame) + 1));

	return 0;
}

LIB_TEST(lib_test_apple_mtp_frame, 0);

static int lib_test_apple_mtp_init_name(struct unit_test_state *uts)
{
	struct apple_mtp_init_header init = {
		.type = APPLE_MTP_EVENT_INIT,
		.iface = 5,
		.name = "keyboard",
		.more_packets = 1,
	};
	char name[APPLE_MTP_MAX_NAME + 1];
	bool more_packets;
	u8 iface;

	ut_assertok(apple_mtp_init_name(&init, sizeof(init), &iface, name,
					&more_packets));
	ut_asserteq(5, iface);
	ut_asserteq_str("keyboard", name);
	ut_assert(more_packets);

	memcpy(init.name, "1234567890abcdef", APPLE_MTP_MAX_NAME);
	init.more_packets = 0;
	ut_assertok(apple_mtp_init_name(&init, sizeof(init), &iface, name,
					&more_packets));
	ut_asserteq_str("1234567890abcdef", name);
	ut_assert(!more_packets);

	ut_asserteq(-EMSGSIZE,
		    apple_mtp_init_name(&init, sizeof(init) - 1, &iface, name,
					&more_packets));
	init.type = APPLE_MTP_EVENT_READY;
	ut_asserteq(-EPROTO,
		    apple_mtp_init_name(&init, sizeof(init), &iface, name,
					&more_packets));
	init.type = APPLE_MTP_EVENT_INIT;
	init.name[0] = '\0';
	ut_asserteq(-EINVAL,
		    apple_mtp_init_name(&init, sizeof(init), &iface, name,
					&more_packets));

	return 0;
}

LIB_TEST(lib_test_apple_mtp_init_name, 0);

static int lib_test_apple_mtp_neo_interfaces(struct unit_test_state *uts)
{
	/* Complete discovery headers captured on J700, in arrival order. */
	const struct apple_mtp_init_header records[] = {
		{ .type = 0xf0, .reserved = { 1, 0 }, .iface = 1, .name = "multi-touch" },
		{ .type = 0xf0, .reserved = { 1, 0 }, .iface = 3, .name = "keyboard" },
		{ .type = 0xf0, .reserved = { 1, 0 }, .iface = 2, .name = "mtp" },
	};
	char name[APPLE_MTP_MAX_NAME + 1];
	bool more_packets;
	u8 iface;
	int i;

	for (i = 0; i < ARRAY_SIZE(records); i++) {
		ut_assertok(apple_mtp_init_name(&records[i], sizeof(records[i]),
						&iface, name, &more_packets));
		ut_asserteq(records[i].iface, iface);
		ut_asserteq_str(records[i].name, name);
		ut_assert(!more_packets);
		/* Interface 2 must not be silently reinterpreted as STM. */
		ut_assert(strcmp(name, "stm"));
	}
	return 0;
}

LIB_TEST(lib_test_apple_mtp_neo_interfaces, 0);

static int lib_test_apple_mtp_neo_keys(struct unit_test_state *uts)
{
	/* Unmodified J700 FIFO captures: R held, then all keys released. */
	const u8 frames[][32] = {
		{ 0x08, 0x12, 0x14, 0x00, 0xbd, 0x03, 0x00, 0x00,
		  0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x01, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x39, 0xea, 0xe1, 0xea },
		{ 0x08, 0x12, 0x14, 0x00, 0xc2, 0x03, 0x00, 0x00,
		  0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x34, 0xea, 0xe1, 0xff },
	};
	const struct apple_mtp_header *hdr;
	const struct apple_mtp_subheader *sub;
	int i;

	for (i = 0; i < ARRAY_SIZE(frames); i++) {
		hdr = (const void *)frames[i];
		sub = (const void *)(frames[i] + sizeof(*hdr));
		ut_assertok(apple_mtp_validate_frame(frames[i], sizeof(frames[i])));
		ut_asserteq(3, hdr->iface);
		ut_asserteq(APPLE_MTP_CHANNEL_REPORT, hdr->channel);
		ut_asserteq(10, le16_to_cpu(sub->length));
		ut_assert(apple_mtp_keyboard_length_valid(le16_to_cpu(hdr->length),
							le16_to_cpu(sub->length)));
		ut_asserteq(1, frames[i][16]); /* HID report ID */
		ut_asserteq(i ? 0 : 0x15, frames[i][19]);
	}

	/* Preserve the padded M4 format; reject short, odd and oversized input. */
	ut_assert(apple_mtp_keyboard_length_valid(20, 12));
	ut_assert(!apple_mtp_keyboard_length_valid(20, 9));
	ut_assert(!apple_mtp_keyboard_length_valid(20, 11));
	ut_assert(!apple_mtp_keyboard_length_valid(20, 13));
	ut_assert(!apple_mtp_keyboard_length_valid(16, 10));
	ut_assert(!apple_mtp_keyboard_length_valid(24, 10));
	return 0;
}

LIB_TEST(lib_test_apple_mtp_neo_keys, 0);
