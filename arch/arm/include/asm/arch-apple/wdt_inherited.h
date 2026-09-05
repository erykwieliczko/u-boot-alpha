/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __APPLE_WDT_INHERITED_H
#define __APPLE_WDT_INHERITED_H

#if CONFIG_IS_ENABLED(WDT_APPLE_PRESERVE_INHERITED_STATE)
void apple_wdt_quiesce_devices(void);
#else
static inline void apple_wdt_quiesce_devices(void) {}
#endif

#endif
