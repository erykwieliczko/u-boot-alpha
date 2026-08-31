// SPDX-License-Identifier: GPL-2.0+

#include <dm.h>
#include <errno.h>
#include <event.h>
#include <wdt.h>
#include <asm/io.h>

#define APPLE_WDT_CTRL			0x1c
#define APPLE_WDT_CTRL_RESET_EN		BIT(2)
#define APPLE_LINUX_HANDOFF_WDT_MS	10000

static bool inherited_active;

static int apple_wdt_inherited_get_device(struct udevice **devp)
{
	if (!of_machine_is_compatible("apple,j713") ||
	    !of_machine_is_compatible("apple,t8132"))
		return -ENODEV;

	return uclass_get_device_by_driver(UCLASS_WDT,
					   DM_DRIVER_GET(apple_wdt), devp);
}

static int apple_wdt_inherited_adopt(void)
{
	struct udevice *dev;
	void *base;
	int ret;

	ret = apple_wdt_inherited_get_device(&dev);
	if (ret)
		return ret;

	base = dev_read_addr_ptr(dev);
	if (!base)
		return -EINVAL;

	inherited_active =
		readl(base + APPLE_WDT_CTRL) & APPLE_WDT_CTRL_RESET_EN;
	if (!inherited_active)
		return 0;

	return wdt_set_force_autostart(dev);
}

EVENT_SPY_SIMPLE(EVT_DM_POST_INIT_R, apple_wdt_inherited_adopt);

static int apple_wdt_rearm_inherited(u64 timeout_ms)
{
	struct udevice *dev;
	int ret;

	if (!inherited_active)
		return 0;

	ret = apple_wdt_inherited_get_device(&dev);
	if (ret)
		return ret;

	return wdt_start(dev, timeout_ms, 0);
}

void board_quiesce_devices(void)
{
	int ret;

	ret = apple_wdt_rearm_inherited(APPLE_LINUX_HANDOFF_WDT_MS);
	if (ret)
		printf("WDT:   Failed to rearm inherited Apple watchdog (%d)\n",
		       ret);
}
