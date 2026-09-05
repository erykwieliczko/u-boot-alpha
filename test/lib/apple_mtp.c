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
