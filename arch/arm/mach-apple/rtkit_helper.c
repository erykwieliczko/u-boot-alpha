// SPDX-License-Identifier: GPL-2.0+ OR MIT
/*
 * Copyright The Asahi Linux Contributors
 */

#include <dm.h>
#include <fdt_support.h>
#include <mailbox.h>
#include <mapmem.h>
#include <reset.h>

#include <asm/io.h>
#include <asm/arch/rtkit.h>
#include <linux/iopoll.h>
#include <linux/sizes.h>

DECLARE_GLOBAL_DATA_PTR;

/* ASC registers */
#define REG_CPU_CTRL		0x0044
#define  REG_CPU_CTRL_RUN	BIT(4)

#define APPLE_RTKIT_EP_OSLOG 8

struct rtkit_helper_priv {
	void *asc;		/* ASC registers */
	struct mbox_chan chan;
	struct apple_rtkit *rtk;
	bool sram_stolen;
	bool handoff_capable;
	bool retain;
};

static int shmem_setup(void *cookie, struct apple_rtkit_buffer *buf)
{
	struct udevice *dev = cookie;
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	int ret;

	if (!buf->is_mapped) {
		/*
		 * Special case: The OSLog buffer on MTP persists on Linux handoff.
		 * Steal some SRAM instead of putting this in DRAM, so we don't
		 * have to hand off DART/DAPF mappings.
		 */
		if (buf->endpoint == APPLE_RTKIT_EP_OSLOG) {
			if (priv->sram_stolen) {
				printf("%s: Tried to map more than one OSLog buffer out of SRAM\n",
				       __func__);
			} else {
				fdt_size_t size;
				fdt_addr_t addr;

				addr = dev_read_addr_size_name(dev, "sram", &size);

				if (addr != FDT_ADDR_T_NONE) {
					buf->dva = ALIGN_DOWN(addr + size - buf->size, SZ_16K);
					priv->sram_stolen = true;

					return 0;
				} else {
					printf("%s: No SRAM, falling back to DRAM\n", __func__);
				}
			}
		}

		buf->buffer = memalign(SZ_16K, ALIGN(buf->size, SZ_16K));
		if (!buf->buffer)
			return -ENOMEM;
		if (priv->handoff_capable) {
			ret = fdt_add_mem_rsv((void *)gd->fdt_blob,
					      (phys_addr_t)buf->buffer,
					      ALIGN(buf->size, SZ_16K));
			if (ret) {
				printf("%s: failed to reserve RTKit endpoint %u buffer: %s\n",
				       __func__, buf->endpoint, fdt_strerror(ret));
				free(buf->buffer);
				buf->buffer = NULL;
				return -ENOSPC;
			}
		}

		buf->dva = (u64)buf->buffer;
	}
	return 0;
}

static void shmem_destroy(void *cookie, struct apple_rtkit_buffer *buf)
{
	struct udevice *dev = cookie;
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	void *fdt = (void *)gd->fdt_blob;
	u64 addr, size;
	int i;

	if (!buf->buffer)
		return;

	if (priv->handoff_capable) {
		for (i = fdt_num_mem_rsv(fdt) - 1; i >= 0; i--) {
			if (fdt_get_mem_rsv(fdt, i, &addr, &size))
				continue;
			if (addr == (phys_addr_t)buf->buffer &&
			    size == ALIGN(buf->size, SZ_16K)) {
				fdt_del_mem_rsv(fdt, i);
				break;
			}
		}
	}

	if (buf->buffer)
		free(buf->buffer);
}

static int rtkit_helper_probe(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	bool running;
	u32 ctrl;
	int ret;

	priv->asc = dev_read_addr_ptr(dev);
	if (!priv->asc)
		return -EINVAL;

	/*
	 * T8132 firmware cannot be restarted at the OS boundary. Keep this as
	 * compatible data so later SoCs can opt in without board-name checks.
	 */
	priv->handoff_capable =
		device_is_compatible(dev, "apple,t8132-rtk-helper-asc4");

	ret = mbox_get_by_index(dev, 0, &priv->chan);
	if (ret < 0)
		return ret;

	ctrl = readl(priv->asc + REG_CPU_CTRL);
	running = ctrl & REG_CPU_CTRL_RUN;
	writel(ctrl | REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);

	priv->rtk = apple_rtkit_init(&priv->chan, dev, shmem_setup, shmem_destroy);
	if (!priv->rtk)
		return -ENOMEM;

	ret = running && priv->handoff_capable ?
		apple_rtkit_wake(priv->rtk) : apple_rtkit_boot(priv->rtk);
	if (ret < 0) {
		printf("%s: Helper RTKit %s returned: %d\n", __func__,
		       running && priv->handoff_capable ? "wake" : "boot", ret);
		goto err_free;
	}

	ret = apple_rtkit_set_ap_power(priv->rtk, APPLE_RTKIT_PWR_STATE_ON);
	if (ret < 0) {
		printf("%s: Helper apple_rtkit_set_ap_power returned: %d\n", __func__, ret);
		goto err_free;
	}

	return 0;

err_free:
	apple_rtkit_free(priv->rtk);
	priv->rtk = NULL;
	return ret;
}

static int rtkit_helper_remove(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	u32 ctrl;

	if (priv->retain)
		return 0;

	apple_rtkit_shutdown(priv->rtk, APPLE_RTKIT_PWR_STATE_QUIESCED);

	ctrl = readl(priv->asc + REG_CPU_CTRL);
	writel(ctrl & ~REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);

	apple_rtkit_free(priv->rtk);
	priv->rtk = NULL;

	return 0;
}

int apple_rtkit_helper_poll(struct udevice *dev, ulong timeout)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);

	return apple_rtkit_poll(priv->rtk, timeout);
}

bool apple_rtkit_helper_can_retain(struct udevice *dev)
{
	struct rtkit_helper_priv *priv;

	if (!dev || !device_active(dev))
		return false;

	priv = dev_get_priv(dev);
	return priv->handoff_capable;
}

int apple_rtkit_helper_retain(struct udevice *dev)
{
	struct rtkit_helper_priv *priv;

	if (!dev || !device_active(dev))
		return -ENODEV;

	priv = dev_get_priv(dev);
	if (!priv->rtk)
		return -ENODEV;
	if (!priv->handoff_capable)
		return -EOPNOTSUPP;

	priv->retain = true;
	dev_or_flags(dev, DM_FLAG_LEAVE_PD_ON);
	return 0;
}

void apple_rtkit_helper_release(struct udevice *dev)
{
	struct rtkit_helper_priv *priv;

	if (!dev || !device_active(dev))
		return;

	priv = dev_get_priv(dev);
	priv->retain = false;
	dev_bic_flags(dev, DM_FLAG_LEAVE_PD_ON);
}

static const struct udevice_id rtkit_helper_ids[] = {
	{ .compatible = "apple,rtk-helper-asc4" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(rtkit_helper) = {
	.name = "rtkit_helper",
	.id = UCLASS_MISC,
	.of_match = rtkit_helper_ids,
	.priv_auto = sizeof(struct rtkit_helper_priv),
	.probe = rtkit_helper_probe,
	.remove = rtkit_helper_remove,
	.flags = DM_FLAG_OS_PREPARE,
};
