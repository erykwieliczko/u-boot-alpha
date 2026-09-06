// SPDX-License-Identifier: GPL-2.0+ OR MIT
/*
 * Copyright The Asahi Linux Contributors
 */

#include <dm.h>
#include <fdt_support.h>
#include <mailbox.h>
#include <mapmem.h>
#include <linux/list.h>
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

struct rtkit_handoff_buffer {
	struct list_head node;
	void *buffer;
	size_t size;
};

struct rtkit_helper_priv {
	void *asc;		/* ASC registers */
	struct mbox_chan chan;
	struct apple_rtkit *rtk;
	bool sram_stolen;
	bool handoff_capable;
	bool retain;
	struct list_head handoff_buffers;
};

static int shmem_setup(void *cookie, struct apple_rtkit_buffer *buf)
{
	struct udevice *dev = cookie;
	struct rtkit_helper_priv *priv = dev_get_priv(dev);

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
			struct rtkit_handoff_buffer *entry = malloc(sizeof(*entry));

			if (!entry) {
				free(buf->buffer);
				buf->buffer = NULL;
				return -ENOMEM;
			}
			entry->buffer = buf->buffer;
			entry->size = ALIGN(buf->size, SZ_16K);
			list_add_tail(&entry->node, &priv->handoff_buffers);
		}

		buf->dva = (u64)buf->buffer;
	}
	return 0;
}

static void shmem_destroy(void *cookie, struct apple_rtkit_buffer *buf)
{
	struct udevice *dev = cookie;
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	struct rtkit_handoff_buffer *entry, *next;

	if (!buf->buffer)
		return;

	list_for_each_entry_safe(entry, next, &priv->handoff_buffers, node) {
		if (entry->buffer == buf->buffer) {
			list_del(&entry->node);
			free(entry);
			break;
		}
	}
	free(buf->buffer);
	buf->buffer = NULL;
}

static int rtkit_helper_probe(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	bool running, wake;
	u32 ctrl;
	int ret;

	INIT_LIST_HEAD(&priv->handoff_buffers);
	priv->asc = dev_read_addr_ptr(dev);
	if (!priv->asc)
		return -EINVAL;

	/*
	 * Keep the working T8132/T8140 sessions at the OS boundary. This is
	 * independent of whether initial discovery needs wake (T8132) or boot.
	 */
	priv->handoff_capable =
		device_is_compatible(dev, "apple,t8132-rtk-helper-asc4") ||
		device_is_compatible(dev, "apple,t8140-rtk-helper-asc4");

	ret = mbox_get_by_index(dev, 0, &priv->chan);
	if (ret < 0)
		return ret;

	ctrl = readl(priv->asc + REG_CPU_CTRL);
	running = ctrl & REG_CPU_CTRL_RUN;
	writel(ctrl | REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);

	priv->rtk = apple_rtkit_init(&priv->chan, dev, shmem_setup, shmem_destroy);
	if (!priv->rtk)
		return -ENOMEM;

	wake = running && device_is_compatible(dev, "apple,t8132-rtk-helper-asc4");
	ret = wake ?
		apple_rtkit_wake(priv->rtk) : apple_rtkit_boot(priv->rtk);
	if (ret < 0) {
		printf("%s: Helper RTKit %s returned: %d\n", __func__,
		       wake ? "wake" : "boot", ret);
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
	int ret;

	if (!dev || !device_active(dev))
		return -ENODEV;

	priv = dev_get_priv(dev);
	if (!priv->rtk)
		return -ENODEV;
	if (!priv->handoff_capable)
		return -EOPNOTSUPP;
	if (device_is_compatible(dev, "apple,t8140-rtk-helper-asc4")) {
		if (!dev->iommu || !device_is_compatible(dev->iommu, "apple,t8140-dart"))
			return -ENODEV;
		ret = ofnode_write_u32(dev_ofnode(dev->iommu),
				       "linux-enablement-mac,retained-bypass", 1);
		if (ret)
			return ret;
	}

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
	if (dev->iommu && device_is_compatible(dev, "apple,t8140-rtk-helper-asc4"))
		ofnode_write_u32(dev_ofnode(dev->iommu),
				 "linux-enablement-mac,retained-bypass", 0);
	dev_bic_flags(dev, DM_FLAG_LEAVE_PD_ON);
}

/* Publish retained allocations only to the OS copy, never the control FDT.
 * Resizing the flat control tree invalidates bound device offsets and names.
 */
int apple_rtkit_helper_fixup_fdt(struct udevice *dev, void *fdt)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	struct rtkit_handoff_buffer *entry;
	int ret;

	if (fdt == gd->fdt_blob)
		return -EINVAL;
	if (!priv->retain)
		return 0;

	list_for_each_entry(entry, &priv->handoff_buffers, node) {
		ret = fdt_add_mem_rsv(fdt, (phys_addr_t)entry->buffer, entry->size);
		if (ret)
			return ret;
	}

	return 0;
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
