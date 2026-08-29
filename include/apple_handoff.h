/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __APPLE_HANDOFF_H
#define __APPLE_HANDOFF_H

#if CONFIG_IS_ENABLED(WDT_APPLE_PRESERVE_INHERITED_STATE)
int apple_wdt_handoff_rearm(void);
#else
static inline int apple_wdt_handoff_rearm(void)
{
	return 0;
}
#endif

#endif
