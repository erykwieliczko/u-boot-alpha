// SPDX-License-Identifier: GPL-2.0+

#include <dm.h>
#include <env.h>
#include <event.h>
#include <mapmem.h>
#include <malloc.h>
#include <stdio_dev.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/unaligned.h>
#include <linux/build_bug.h>
#include <linux/compat.h>
#include <linux/iopoll.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <linux/usb/gadget.h>
#include <usb/apple_dwc3_handoff.h>

#include "core.h"

#define APPLE_DWC3_HANDOFF_PROPERTY \
	"linux-enablement-mac,m1n1-dwc3-handoff"
#define APPLE_DWC3_STDIO_NAME		"m1n1_dwc3"
#define APPLE_DWC3_TX_CHUNK		511
#define APPLE_DWC3_TX_TIMEOUT_US		250000

struct apple_dwc3_handoff_state {
	struct apple_dwc3_handoff_raw *raw;
	struct apple_dwc3_handoff_desc desc;
	void __iomem *regs;
	u32 *event_buffer;
	struct dwc3_trb *tx_trb;
	struct dwc3_trb *rx_trb;
	u8 *tx_buffer;
	u8 *rx_buffer;
	u32 event_cursor;
	u32 rx_offset;
	u32 rx_length;
	bool tx_busy;
	bool rx_busy;
	bool active;
	bool io_failed;
	bool stdio_registered;
};

static struct apple_dwc3_handoff_state handoff;

static_assert(sizeof(struct apple_dwc3_handoff_raw) == 192);
static_assert(offsetof(struct apple_dwc3_handoff_raw, flags) == 0x10);
static_assert(offsetof(struct apple_dwc3_handoff_raw, event_buffer_phys) == 0x20);
static_assert(offsetof(struct apple_dwc3_handoff_raw, tx_buffer_phys) == 0x60);
static_assert(offsetof(struct apple_dwc3_handoff_raw, rx_buffer_phys) == 0x90);
static_assert(offsetof(struct apple_dwc3_handoff_raw, rx_busy) == 0xbc);

DECLARE_GLOBAL_DATA_PTR;

static const struct apple_dwc3_handoff_region j713_mmio[] = {
	{ 0x402280000ULL, APPLE_DWC3_REGS_SIZE },
};

static void handoff_cache_invalidate(const void *address, size_t size)
{
	ulong start = ALIGN_DOWN((ulong)address, ARCH_DMA_MINALIGN);
	ulong end = ALIGN((ulong)address + size, ARCH_DMA_MINALIGN);

	invalidate_dcache_range(start, end);
}

static void handoff_cache_flush(const void *address, size_t size)
{
	ulong start = ALIGN_DOWN((ulong)address, ARCH_DMA_MINALIGN);
	ulong end = ALIGN((ulong)address + size, ARCH_DMA_MINALIGN);

	flush_dcache_range(start, end);
}

static bool runtime_range_contains(u64 outer_start, u64 outer_size,
				   u64 inner_start, u64 inner_size)
{
	u64 outer_end, inner_end;

	if (!outer_size || !inner_size || outer_size > U64_MAX - outer_start ||
	    inner_size > U64_MAX - inner_start)
		return false;

	outer_end = outer_start + outer_size;
	inner_end = inner_start + inner_size;

	return inner_start >= outer_start && inner_end <= outer_end;
}

static bool runtime_range_in_regions(const struct apple_dwc3_handoff_region *regions,
				     size_t count, u64 start, u64 size)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (runtime_range_contains(regions[i].start, regions[i].size,
					   start, size))
			return true;
	}

	return false;
}

static int handoff_get_limits(struct apple_dwc3_handoff_limits *limits,
			      struct apple_dwc3_handoff_region *ram,
			      struct apple_dwc3_handoff_region **reservedp)
{
	struct apple_dwc3_handoff_region *reserved;
	fdt_size_t wdt_size;
	fdt_addr_t wdt_address;
	ofnode wdt_node;
	int count, i;

	if (!of_machine_is_compatible("apple,j713") ||
	    !of_machine_is_compatible("apple,t8132"))
		return -ENODEV;

	memset(limits, 0, sizeof(*limits));
	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		if (!gd->bd->bi_dram[i].size)
			continue;
		ram[limits->ram_count].start = gd->bd->bi_dram[i].start;
		ram[limits->ram_count].size = gd->bd->bi_dram[i].size;
		limits->ram_count++;
	}
	if (!limits->ram_count)
		return -EINVAL;

	count = fdt_num_mem_rsv(gd->fdt_blob);
	if (count <= 0)
		return -EINVAL;

	reserved = calloc(count, sizeof(*reserved));
	if (!reserved)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		if (fdt_get_mem_rsv(gd->fdt_blob, i, &reserved[i].start,
				    &reserved[i].size)) {
			free(reserved);
			return -EINVAL;
		}
	}

	wdt_node = ofnode_by_compatible(ofnode_null(), "apple,t8132-wdt");
	if (!ofnode_valid(wdt_node)) {
		free(reserved);
		return -EINVAL;
	}

	wdt_address = ofnode_get_addr_size_index(wdt_node, 0, &wdt_size);
	if (wdt_address == FDT_ADDR_T_NONE || wdt_size < 0x20) {
		free(reserved);
		return -EINVAL;
	}

	limits->ram = ram;
	limits->reserved = reserved;
	limits->reserved_count = count;
	limits->mmio = j713_mmio;
	limits->mmio_count = ARRAY_SIZE(j713_mmio);
	limits->expected_wdt_regs = wdt_address;
	*reservedp = reserved;

	return 0;
}

static void handoff_publish_state(void)
{
	handoff.raw->event_buffer_offset = cpu_to_le32(handoff.event_cursor);
	handoff.raw->tx_busy = cpu_to_le32(handoff.tx_busy);
	handoff.raw->rx_busy = cpu_to_le32(handoff.rx_busy);
	handoff_cache_flush(handoff.raw, sizeof(*handoff.raw));
	/* Make the complete runtime snapshot visible to the next stage. */
	wmb();
}

static int handoff_endpoint_command(u8 endpoint, u32 command, u64 trb_iova)
{
	void __iomem *cmd_reg = handoff.regs + DWC3_DEPCMD(endpoint);
	u32 value;
	int ret;

	writel(upper_32_bits(trb_iova),
	       handoff.regs + DWC3_DEPCMDPAR0(endpoint));
	writel(lower_32_bits(trb_iova),
	       handoff.regs + DWC3_DEPCMDPAR1(endpoint));
	writel(0, handoff.regs + DWC3_DEPCMDPAR2(endpoint));
	writel(command | DWC3_DEPCMD_CMDACT, cmd_reg);

	ret = readl_poll_timeout(cmd_reg, value,
				 !(value & DWC3_DEPCMD_CMDACT), 1000);
	if (ret)
		return ret;

	return DWC3_DEPCMD_STATUS(value) ? -EIO : 0;
}

static int handoff_start_transfer(u8 endpoint, struct dwc3_trb *trb,
				  u64 trb_iova, u64 buffer_iova, u32 length)
{
	trb->bpl = lower_32_bits(buffer_iova);
	trb->bph = upper_32_bits(buffer_iova);
	trb->size = DWC3_TRB_SIZE_LENGTH(length);
	trb->ctrl = DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_LST |
		    DWC3_TRB_CTRL_ISP_IMI | DWC3_TRBCTL_NORMAL;
	handoff_cache_flush(trb, sizeof(*trb));
	/* The DWC3 must observe the completed TRB before STARTTRANSFER. */
	wmb();

	return handoff_endpoint_command(endpoint, DWC3_DEPCMD_STARTTRANSFER,
					trb_iova);
}

static void handoff_fail_io(void)
{
	handoff.io_failed = true;
}

static void handoff_handle_event(union dwc3_event event)
{
	if (event.type.is_devspec) {
		if (event.type.type == DWC3_EVENT_TYPE_DEV &&
		    (event.devt.type == DWC3_DEVICE_EVENT_DISCONNECT ||
		     event.devt.type == DWC3_DEVICE_EVENT_RESET))
			handoff_fail_io();
		return;
	}

	if (event.depevt.endpoint_event != DWC3_DEPEVT_XFERCOMPLETE)
		return;
	if (event.depevt.status & DEPEVT_STATUS_BUSERR) {
		handoff_fail_io();
		return;
	}

	if (event.depevt.endpoint_number == handoff.desc.tx_endpoint) {
		handoff_cache_invalidate(handoff.tx_trb, sizeof(*handoff.tx_trb));
		/* Read the TRB only after invalidation has completed. */
		rmb();
		handoff.tx_busy = false;
	} else if (event.depevt.endpoint_number == handoff.desc.rx_endpoint) {
		u32 remaining;

		handoff_cache_invalidate(handoff.rx_trb, sizeof(*handoff.rx_trb));
		/* Read transfer residue only after invalidation has completed. */
		rmb();
		remaining = handoff.rx_trb->size & DWC3_TRB_SIZE_MASK;
		if (remaining > handoff.desc.tx_max_packet) {
			handoff_fail_io();
			return;
		}

		handoff.rx_busy = false;
		handoff.rx_offset = 0;
		handoff.rx_length = handoff.desc.tx_max_packet - remaining;
		if (handoff.rx_length) {
			handoff_cache_invalidate(handoff.rx_buffer,
						 handoff.rx_length);
			/* Make received bytes visible before exposing their length. */
			rmb();
		}
	}
}

static void handoff_handle_events(void)
{
	u32 event_size = handoff.desc.event_buffer_size;
	u32 pending;

	if (!handoff.active || handoff.io_failed)
		return;

	pending = readl(handoff.regs + DWC3_GEVNTCOUNT(0)) &
		  DWC3_GEVNTCOUNT_MASK;
	if ((pending & 3) || pending > event_size) {
		handoff_fail_io();
		return;
	}

	while (pending) {
		union dwc3_event event;
		u32 *event_word = &handoff.event_buffer[handoff.event_cursor];
		u32 next_event;

		handoff_cache_invalidate(event_word, sizeof(*event_word));
		/* Read the event only after invalidation has completed. */
		rmb();
		event.raw = *event_word;
		if (!event.type.is_devspec &&
		    event.depevt.endpoint_number != handoff.desc.tx_endpoint &&
		    event.depevt.endpoint_number != handoff.desc.rx_endpoint) {
			handoff_fail_io();
			return;
		}

		next_event = apple_dwc3_next_event(handoff.event_cursor, event_size);
		handoff.event_cursor = next_event;
		writel(sizeof(u32), handoff.regs + DWC3_GEVNTCOUNT(0));
		pending -= sizeof(u32);

		handoff_handle_event(event);
		handoff_publish_state();
		if (handoff.io_failed)
			return;
	}
}

static int handoff_wait_for_tx(void)
{
	ulong start = timer_get_us();

	while (handoff.tx_busy && !handoff.io_failed) {
		handoff_handle_events();
		if (timer_get_us() - start >= APPLE_DWC3_TX_TIMEOUT_US) {
			handoff_fail_io();
			return -ETIMEDOUT;
		}
	}

	return handoff.io_failed ? -EIO : 0;
}

static int handoff_start_rx(void)
{
	int ret;

	if (handoff.rx_busy || handoff.rx_length || handoff.io_failed)
		return 0;

	handoff_cache_flush(handoff.rx_buffer, handoff.desc.tx_max_packet);
	ret = handoff_start_transfer(handoff.desc.rx_endpoint, handoff.rx_trb,
				     handoff.desc.rx_trb_iova,
				     handoff.desc.rx_buffer_iova,
				     handoff.desc.tx_max_packet);
	if (ret) {
		handoff_fail_io();
		return ret;
	}

	handoff.rx_busy = true;
	handoff_publish_state();
	return 0;
}

static size_t handoff_write(const u8 *source, size_t count)
{
	size_t written = 0;

	while (written < count) {
		size_t length;

		if (handoff_wait_for_tx())
			break;

		length = min(count - written, (size_t)APPLE_DWC3_TX_CHUNK);
		memcpy(handoff.tx_buffer, source + written, length);
		handoff_cache_flush(handoff.tx_buffer, length);
		if (handoff_start_transfer(handoff.desc.tx_endpoint,
					   handoff.tx_trb,
					   handoff.desc.tx_trb_iova,
					   handoff.desc.tx_buffer_iova,
					   length)) {
			handoff_fail_io();
			break;
		}

		handoff.tx_busy = true;
		handoff_publish_state();
		written += length;
	}

	return written;
}

static int handoff_stdio_tstc(struct stdio_dev *dev)
{
	handoff_handle_events();
	if (!handoff.rx_length)
		handoff_start_rx();

	return handoff.rx_length != 0;
}

static int handoff_stdio_getc(struct stdio_dev *dev)
{
	u8 value;

	if (!handoff.rx_length)
		return -EAGAIN;

	value = handoff.rx_buffer[handoff.rx_offset++];
	handoff.rx_length--;
	if (!handoff.rx_length) {
		handoff.rx_offset = 0;
		handoff_start_rx();
	}

	return value;
}

static void handoff_stdio_putc(struct stdio_dev *dev, const char value)
{
	if (value == '\n')
		handoff_write((const u8 *)"\r", 1);
	handoff_write((const u8 *)&value, 1);
}

static void handoff_stdio_puts(struct stdio_dev *dev, const char *string)
{
	const char *start = string;
	const char *newline;

	while ((newline = strchr(start, '\n'))) {
		handoff_write((const u8 *)start, newline - start);
		handoff_write((const u8 *)"\r\n", 2);
		start = newline + 1;
	}
	handoff_write((const u8 *)start, strlen(start));
}

#ifdef CONFIG_CONSOLE_FLUSH_SUPPORT
static void handoff_stdio_flush(struct stdio_dev *dev)
{
	handoff_wait_for_tx();
}
#endif

static bool console_list_has(const char *list, const char *name)
{
	size_t name_length = strlen(name);

	while (list && *list) {
		const char *end = strchr(list, ',');
		size_t length = end ? end - list : strlen(list);

		if (length == name_length && !memcmp(list, name, length))
			return true;
		if (!end)
			break;
		list = end + 1;
	}

	return false;
}

static int console_list_append(const char *variable, const char *name)
{
	const char *current = env_get(variable);
	char *updated;
	int ret;

	if (console_list_has(current, name))
		return 0;
	if (!current || !*current)
		return env_set(variable, name);

	updated = malloc(strlen(current) + strlen(name) + 2);
	if (!updated)
		return -ENOMEM;

	sprintf(updated, "%s,%s", current, name);
	ret = env_set(variable, updated);
	free(updated);
	return ret;
}

static int handoff_register_stdio(void)
{
	struct stdio_dev dev = {
		.name = APPLE_DWC3_STDIO_NAME,
		.flags = DEV_FLAGS_INPUT | DEV_FLAGS_OUTPUT,
		.putc = handoff_stdio_putc,
		.puts = handoff_stdio_puts,
		.getc = handoff_stdio_getc,
		.tstc = handoff_stdio_tstc,
	};
	int ret;

	STDIO_DEV_ASSIGN_FLUSH(&dev, handoff_stdio_flush);
	ret = stdio_register(&dev);
	if (ret)
		return ret;

	handoff.stdio_registered = true;
	ret = console_list_append("stdin", APPLE_DWC3_STDIO_NAME);
	ret = console_list_append("stdout", APPLE_DWC3_STDIO_NAME) ?: ret;
	ret = console_list_append("stderr", APPLE_DWC3_STDIO_NAME) ?: ret;

	return ret;
}

static int handoff_validate_hardware(void)
{
	u32 endpoints;

	endpoints = DWC3_DALEPENA_EP(handoff.desc.tx_endpoint) |
		    DWC3_DALEPENA_EP(handoff.desc.rx_endpoint);

	if ((readl(handoff.regs + DWC3_GSNPSID) & DWC3_GSNPSID_MASK) !=
							0x33310000 ||
	    !(readl(handoff.regs + DWC3_DCTL) & DWC3_DCTL_RUN_STOP) ||
	    (readl(handoff.regs + DWC3_DALEPENA) & endpoints) != endpoints ||
	    (readl(handoff.regs + DWC3_GEVNTSIZ(0)) & 0xffff) !=
					 handoff.desc.event_buffer_size)
		return -EINVAL;

	return 0;
}

static int handoff_adopt(void)
{
	struct apple_dwc3_handoff_region ram[CONFIG_NR_DRAM_BANKS];
	struct apple_dwc3_handoff_region *reserved = NULL;
	struct apple_dwc3_handoff_limits limits;
	const void *property;
	u64 descriptor_phys;
	ofnode chosen;
	int length;
	int ret;

	chosen = ofnode_path("/chosen");
	if (!ofnode_valid(chosen))
		return 0;

	property = ofnode_get_property(chosen, APPLE_DWC3_HANDOFF_PROPERTY,
				       &length);
	if (!property)
		return 0;
	if (length != sizeof(u64))
		return -EINVAL;

	descriptor_phys = get_unaligned_be64(property);
	if (descriptor_phys & (APPLE_DWC3_HANDOFF_ALIGNMENT - 1))
		return -EINVAL;

	ret = handoff_get_limits(&limits, ram, &reserved);
	if (ret)
		return ret;

	if (!runtime_range_in_regions(limits.ram, limits.ram_count,
				      descriptor_phys,
				      sizeof(struct apple_dwc3_handoff_raw)) ||
	    !runtime_range_in_regions(limits.reserved, limits.reserved_count,
				      descriptor_phys,
				      sizeof(struct apple_dwc3_handoff_raw))) {
		ret = -EINVAL;
		goto out;
	}

	handoff.raw = map_sysmem(descriptor_phys, sizeof(*handoff.raw));
	handoff_cache_invalidate(handoff.raw, sizeof(*handoff.raw));
	/* Take the descriptor snapshot only after invalidation completes. */
	rmb();
	ret = apple_dwc3_handoff_validate(handoff.raw, descriptor_phys,
					  &limits, &handoff.desc);
	if (ret)
		goto out;

	handoff.regs = map_sysmem(handoff.desc.regs_phys,
				  APPLE_DWC3_REGS_SIZE);
	handoff.event_buffer = map_sysmem(handoff.desc.event_buffer_phys,
					  handoff.desc.event_buffer_size);
	handoff.tx_buffer = map_sysmem(handoff.desc.tx_buffer_phys,
				       handoff.desc.tx_max_packet);
	handoff.rx_buffer = map_sysmem(handoff.desc.rx_buffer_phys,
				       handoff.desc.tx_max_packet);
	handoff.tx_trb = map_sysmem(handoff.desc.tx_trb_phys,
				    sizeof(*handoff.tx_trb));
	handoff.rx_trb = map_sysmem(handoff.desc.rx_trb_phys,
				    sizeof(*handoff.rx_trb));

	ret = handoff_validate_hardware();
	if (ret)
		goto out;

	handoff.event_cursor = handoff.desc.event_buffer_offset;
	handoff.tx_busy = !!handoff.desc.tx_busy;
	handoff.rx_busy = !!handoff.desc.rx_busy;
	handoff.active = true;

	handoff_handle_events();
	if (!handoff.rx_busy && !handoff.rx_length)
		handoff_start_rx();
	if (handoff.io_failed) {
		handoff.active = false;
		ret = -EIO;
		goto out;
	}

	handoff_publish_state();
	ret = 1;
out:
	free(reserved);
	return ret;
}

int apple_dwc3_handoff_adopt_event(void)
{
	int ret = handoff_adopt();

	if (ret < 0)
		log_warning("DWC3: invalid inherited console handoff (%d)\n", ret);
	else if (ret > 0)
		log_info("DWC3: adopted inherited console\n");

	return 0;
}

EVENT_SPY_SIMPLE(EVT_DM_POST_INIT_R, apple_dwc3_handoff_adopt_event);

int apple_dwc3_handoff_settings_event(void)
{
	int ret;

	if (!handoff.active || handoff.io_failed || handoff.stdio_registered)
		return 0;

	ret = handoff_register_stdio();
	if (ret)
		log_warning("DWC3: failed to register inherited console (%d)\n",
			    ret);

	return 0;
}

EVENT_SPY_SIMPLE(EVT_SETTINGS_R, apple_dwc3_handoff_settings_event);

int apple_dwc3_handoff_pre_probe(void *ctx, struct event *event)
{
	struct udevice *dev = event->data.dm.dev;

	if (!handoff.active || !dev_has_ofnode(dev))
		return 0;

	if (dev_read_addr(dev) == handoff.desc.regs_phys)
		return -EBUSY;

	return 0;
}

EVENT_SPY_FULL(EVT_DM_PRE_PROBE, apple_dwc3_handoff_pre_probe);

bool apple_dwc3_handoff_active(void)
{
	return handoff.active && !handoff.io_failed;
}

void apple_dwc3_handoff_quiesce(void)
{
	if (!handoff.active)
		return;

	handoff_wait_for_tx();
	handoff_handle_events();
	handoff_publish_state();
}
