// SPDX-License-Identifier: GPL-2.0+

#include <apple_handoff.h>
#include <stdio.h>
#include <usb/apple_dwc3_handoff.h>

void board_quiesce_devices(void)
{
	int ret;

	apple_dwc3_handoff_quiesce();

	ret = apple_wdt_handoff_rearm();
	if (ret)
		printf("WDT:   Failed to rearm inherited Apple watchdog (%d)\n",
		       ret);
}
