// SPDX-License-Identifier: GPL-2.0+

#include <test/lib.h>
#include <test/test.h>
#include <test/ut.h>
#include <video.h>

static int lib_test_video_x2r10g10b10(struct unit_test_state *uts)
{
	unsigned int component;

	ut_asserteq(0, video_component_8_to_10(0));
	ut_asserteq(1023, video_component_8_to_10(255));
	ut_asserteq(0, video_component_10_to_8(0));
	ut_asserteq(255, video_component_10_to_8(1023));
	ut_asserteq(128, video_component_10_to_8(512));

	for (component = 0; component <= 255; component++) {
		u16 native = video_component_8_to_10(component);

		ut_asserteq(component, video_component_10_to_8(native));
	}

	ut_asserteq(0x3ff00000, video_pack_x2r10g10b10(255, 0, 0));
	ut_asserteq(0x000ffc00, video_pack_x2r10g10b10(0, 255, 0));
	ut_asserteq(0x000003ff, video_pack_x2r10g10b10(0, 0, 255));
	ut_asserteq(0x3fffffff, video_pack_x2r10g10b10(255, 255, 255));

	return 0;
}

LIB_TEST(lib_test_video_x2r10g10b10, 0);
