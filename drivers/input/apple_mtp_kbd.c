// SPDX-License-Identifier: GPL-2.0+ OR MIT
/*
 * Copyright The Asahi Linux Contributors
 */

#include <apple_mtp.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <dm/simple_bus.h>
#include <keyboard.h>
#include <limits.h>
#include <malloc.h>
#include <stdio_dev.h>
#include <asm/arch/rtkit.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/libfdt.h>
#include "apple_kbd.h"

#define DATA_TX8		0x04
#define DATA_TX_FREE		0x14
#define DATA_RX8		0x1c
#define DATA_RX_COUNT		0x2c

#define MTP_MAX_INTERFACES	16
#define MTP_IO_TIMEOUT_MS	100
#define MTP_COMMAND_TIMEOUT_MS	1000
#define MTP_DISCOVERY_TIMEOUT_MS	2000
#define MTP_DISCOVERY_QUIET_MS	20
#define MTP_DRAIN_TIMEOUT_MS	250
#define MTP_FDT_PATH_SIZE	256
#define MTP_INHERITED_PROP	"linux-enablement-mac,rtkit-inherited"

#define MTP_FEATURE_SET_REPORT	0x80
#define MTP_FEATURE_GET_REPORT	0x81
#define MTP_CMD_ENABLE_INTERFACE	0xb4
#define MTP_STM_REPORT_ID	0x10
#define MTP_STM_REPORT_SERIAL	0x11

struct apple_mtp_iface {
	char name[APPLE_MTP_MAX_NAME + 1];
	char pending_name[APPLE_MTP_MAX_NAME + 1];
	bool name_pending;
	bool discovered;
	bool ready;
};

struct apple_mtp_pending {
	bool active;
	bool done;
	u8 iface;
	u8 flags;
	u8 report;
	u8 seq;
	u8 *response;
	size_t response_capacity;
	size_t response_size;
	int status;
};

struct apple_mtp_kbd_priv {
	struct apple_kbd_priv kbd;
	struct udevice *helper;
	ofnode helper_node;
	void *local;
	void *rmt;

	struct apple_mtp_iface ifaces[MTP_MAX_INTERFACES];
	struct apple_mtp_pending pending;
	u8 stm_iface;
	u8 keyboard_iface;
	u8 touch_iface;
	u8 tx_seq[MTP_MAX_INTERFACES];
	bool retention_capable;
	bool stm_optional;
	bool retained;
	bool handoff_u32;
	bool keyboard_enabled;
	bool shutting_down;

	u8 *frame;
	size_t frame_capacity;
	u8 *init_data;
	size_t init_size;
	u32 fifo_size;
};

static void apple_mtp_poll_rtkit(struct apple_mtp_kbd_priv *priv)
{
	if (priv->helper && device_active(priv->helper))
		apple_rtkit_helper_poll(priv->helper, 0);
}

static int dockchannel_wait_rx(struct apple_mtp_kbd_priv *priv, size_t size)
{
	ulong start = get_timer(0);

	while (readl(priv->local + DATA_RX_COUNT) < size) {
		apple_mtp_poll_rtkit(priv);
		if (get_timer(start) >= MTP_IO_TIMEOUT_MS)
			return -ETIMEDOUT;
		udelay(50);
	}

	return 0;
}

static int dockchannel_read(struct apple_mtp_kbd_priv *priv, void *buf,
			    size_t size)
{
	u8 *p = buf;
	int ret;

	ret = dockchannel_wait_rx(priv, size);
	if (ret)
		return ret;

	while (size--) {
		u8 byte = readl(priv->local + DATA_RX8) >> 8;

		if (p)
			*p++ = byte;
	}

	return 0;
}

static int dockchannel_write(struct apple_mtp_kbd_priv *priv, void *window,
			     const void *buf, size_t size)
{
	const u8 *p = buf;
	ulong start = get_timer(0);

	while (size) {
		u32 free = readl(window + DATA_TX_FREE);

		if (!free) {
			apple_mtp_poll_rtkit(priv);
			if (get_timer(start) >= MTP_IO_TIMEOUT_MS)
				return -ETIMEDOUT;
			udelay(50);
			continue;
		}

		free = min_t(u32, free, size);
		while (free--) {
			writel(*p++, window + DATA_TX8);
			size--;
		}
	}

	return 0;
}

static void dockchannel_drain_bounded(struct apple_mtp_kbd_priv *priv,
				      size_t size)
{
	ulong start = get_timer(0);

	while (size && get_timer(start) < MTP_DRAIN_TIMEOUT_MS) {
		u32 available = readl(priv->local + DATA_RX_COUNT);

		if (!available) {
			apple_mtp_poll_rtkit(priv);
			udelay(50);
			continue;
		}

		available = min_t(u32, available, size);
		while (available--) {
			readl(priv->local + DATA_RX8);
			size--;
		}
	}
}

static int apple_mtp_save_init(struct apple_mtp_kbd_priv *priv,
			       const void *frame, size_t size)
{
	if (size > priv->fifo_size - priv->init_size)
		return -ENOSPC;

	memcpy(priv->init_data + priv->init_size, frame, size);
	priv->init_size += size;
	return 0;
}

static int apple_mtp_handle_init(struct apple_mtp_kbd_priv *priv,
				 const void *payload, size_t size)
{
	struct apple_mtp_iface *iface;
	char name[APPLE_MTP_MAX_NAME + 1];
	bool more_packets;
	u8 index;
	int ret;

	ret = apple_mtp_init_name(payload, size, &index, name, &more_packets);
	if (ret)
		return ret;
	if (!index || index >= MTP_MAX_INTERFACES)
		return -ERANGE;

	iface = &priv->ifaces[index];
	if (more_packets) {
		if (iface->name_pending && strcmp(iface->pending_name, name)) {
			iface->name_pending = false;
			return -EPROTO;
		}
		strlcpy(iface->pending_name, name, sizeof(iface->pending_name));
		iface->name_pending = true;
		return 0;
	}

	if (iface->name_pending) {
		if (strcmp(iface->pending_name, name)) {
			iface->name_pending = false;
			return -EPROTO;
		}
		iface->name_pending = false;
	}

	strlcpy(iface->name, name, sizeof(iface->name));
	iface->discovered = true;
	debug("MTP discovery: interface %u name %s (%zu bytes)\n", index, name, size);
	if (!strcmp(name, "stm"))
		priv->stm_iface = index;
	else if (!strcmp(name, "keyboard"))
		priv->keyboard_iface = index;
	else if (!strcmp(name, "multi-touch"))
		priv->touch_iface = index;

	return 0;
}

static int apple_mtp_handle_ready(struct apple_mtp_kbd_priv *priv,
				  const void *payload, size_t size)
{
	const u8 *event = payload;

	if (size < 2 || event[0] != APPLE_MTP_EVENT_READY)
		return -EMSGSIZE;
	if (!event[1] || event[1] >= MTP_MAX_INTERFACES)
		return -ERANGE;
	if (!priv->ifaces[event[1]].discovered)
		return -ENOENT;

	priv->ifaces[event[1]].ready = true;
	return 0;
}

static void apple_mtp_handle_ack(struct apple_mtp_kbd_priv *priv,
				 const struct apple_mtp_header *hdr,
				 const void *body, size_t body_size)
{
	const struct apple_mtp_subheader *sub = body;
	const u8 *payload = body + sizeof(*sub);
	size_t payload_size;
	size_t response_size;

	if (!priv->pending.active || body_size < sizeof(*sub))
		return;
	payload_size = le16_to_cpu(sub->length);
	if (payload_size > body_size - sizeof(*sub))
		return;
	if (hdr->iface != priv->pending.iface ||
	    hdr->seq != priv->pending.seq ||
	    sub->flags != priv->pending.flags ||
	    !payload_size || payload[0] != priv->pending.report)
		return;

	if (le32_to_cpu(sub->retcode)) {
		priv->pending.status = -EIO;
		priv->pending.done = true;
		return;
	}

	response_size = payload_size - 1;
	if (response_size > priv->pending.response_capacity) {
		priv->pending.status = -EMSGSIZE;
		priv->pending.done = true;
		return;
	}

	if (priv->pending.response && response_size)
		memcpy(priv->pending.response, payload + 1, response_size);
	priv->pending.response_size = response_size;
	priv->pending.status = 0;
	priv->pending.done = true;
}

static int apple_mtp_handle_report(struct apple_mtp_kbd_priv *priv,
				   struct input_config *input,
				   const struct apple_mtp_header *hdr,
				   const void *body, size_t body_size,
				   const void *frame, size_t frame_size)
{
	const struct apple_mtp_subheader *sub = body;
	const u8 *payload = body + sizeof(*sub);
	size_t payload_size;
	int ret;

	if (body_size < sizeof(*sub))
		return -EMSGSIZE;
	payload_size = le16_to_cpu(sub->length);
	if (payload_size > body_size - sizeof(*sub))
		return -EMSGSIZE;
	if (sub->flags || le32_to_cpu(sub->retcode))
		return -EPROTO;

	if (!hdr->iface) {
		if (!payload_size)
			return -EMSGSIZE;

		switch (payload[0]) {
		case APPLE_MTP_EVENT_INIT:
			ret = apple_mtp_handle_init(priv, payload, payload_size);
			if (ret)
				return ret;
			return apple_mtp_save_init(priv, frame, frame_size);
		case APPLE_MTP_EVENT_READY:
			return apple_mtp_handle_ready(priv, payload, payload_size);
		default:
			return 0;
		}
	}

	if (priv->shutting_down || !priv->keyboard_enabled ||
	    hdr->iface != priv->keyboard_iface)
		return 0;
	if (!apple_mtp_keyboard_length_valid(body_size, payload_size))
		return -EPROTO;

	return apple_kbd_handle_report(input, &priv->kbd, (void *)payload,
				       payload_size);
}

static int apple_mtp_process_one(struct apple_mtp_kbd_priv *priv,
				 struct input_config *input)
{
	struct apple_mtp_header *hdr = (void *)priv->frame;
	size_t body_size;
	size_t frame_size;
	int ret;

	if (readl(priv->local + DATA_RX_COUNT) < sizeof(*hdr))
		return 0;

	ret = dockchannel_read(priv, hdr, sizeof(*hdr));
	if (ret)
		return ret;

	body_size = le16_to_cpu(hdr->length);
	frame_size = sizeof(*hdr) + body_size + sizeof(u32);
	if (body_size % sizeof(u32) || frame_size > priv->frame_capacity) {
		size_t drain_size = min(body_size + sizeof(u32),
					(size_t)priv->fifo_size);

		dockchannel_drain_bounded(priv, drain_size);
		return -EMSGSIZE;
	}

	ret = dockchannel_read(priv, priv->frame + sizeof(*hdr),
			       body_size + sizeof(u32));
	if (ret) {
		dockchannel_drain_bounded(priv, body_size + sizeof(u32));
		return ret;
	}

	ret = apple_mtp_validate_frame(priv->frame, frame_size);
	if (ret)
		return ret;

	if (hdr->channel == APPLE_MTP_CHANNEL_COMMAND) {
		apple_mtp_handle_ack(priv, hdr, priv->frame + sizeof(*hdr),
				     body_size);
		return 1;
	}
	if (hdr->channel != APPLE_MTP_CHANNEL_REPORT)
		return -EPROTO;

	ret = apple_mtp_handle_report(priv, input, hdr,
				      priv->frame + sizeof(*hdr), body_size,
				      priv->frame, frame_size);
	return ret < 0 ? ret : 1;
}

static int apple_mtp_send_command(struct apple_mtp_kbd_priv *priv,
				  struct input_config *input, u8 iface,
				  u8 flags, const void *command,
				  size_t command_size, u8 *response,
				  size_t response_capacity,
				  size_t *response_size)
{
	struct apple_mtp_header *hdr = (void *)priv->frame;
	struct apple_mtp_subheader *sub;
	size_t padded_size = ALIGN(command_size, sizeof(u32));
	size_t body_size = sizeof(*sub) + padded_size;
	size_t frame_size = sizeof(*hdr) + body_size + sizeof(u32);
	u8 seq;
	u32 checksum;
	ulong start;
	int ret;

	if (iface >= MTP_MAX_INTERFACES || !command_size ||
	    frame_size > priv->frame_capacity)
		return -EINVAL;

	seq = priv->tx_seq[iface]++;
	memset(priv->frame, 0, frame_size);
	hdr->hdr_len = sizeof(*hdr);
	hdr->channel = APPLE_MTP_CHANNEL_COMMAND;
	hdr->length = cpu_to_le16(body_size);
	hdr->seq = seq;
	hdr->iface = iface;

	sub = (void *)(priv->frame + sizeof(*hdr));
	sub->flags = flags;
	sub->length = cpu_to_le16(command_size);
	memcpy(sub + 1, command, command_size);
	checksum = ~0U -
		apple_mtp_checksum(priv->frame, frame_size - sizeof(u32));
	put_unaligned_le32(checksum, priv->frame + frame_size - sizeof(u32));

	priv->pending = (struct apple_mtp_pending) {
		.active = true,
		.iface = iface,
		.flags = flags,
		.report = *(u8 *)command,
		.seq = seq,
		.response = response,
		.response_capacity = response_capacity,
		.status = -ETIMEDOUT,
	};

	ret = dockchannel_write(priv, priv->local, priv->frame, frame_size);
	if (ret)
		goto done;

	start = get_timer(0);
	while (!priv->pending.done &&
	       get_timer(start) < MTP_COMMAND_TIMEOUT_MS) {
		ret = apple_mtp_process_one(priv, input);
		if (ret <= 0) {
			apple_mtp_poll_rtkit(priv);
			udelay(50);
		}
	}
	ret = priv->pending.done ? priv->pending.status : -ETIMEDOUT;

done:
	if (response_size)
		*response_size = priv->pending.response_size;
	priv->pending.active = false;
	return ret;
}

static int apple_mtp_wait_ready(struct apple_mtp_kbd_priv *priv,
				struct input_config *input, u8 iface)
{
	ulong start = get_timer(0);
	int ret;

	while (!priv->ifaces[iface].ready &&
	       get_timer(start) < MTP_COMMAND_TIMEOUT_MS) {
		ret = apple_mtp_process_one(priv, input);
		if (ret <= 0) {
			apple_mtp_poll_rtkit(priv);
			udelay(50);
		}
	}

	return priv->ifaces[iface].ready ? 0 : -ETIMEDOUT;
}

static bool apple_mtp_discovery_complete(struct apple_mtp_kbd_priv *priv)
{
	if ((!priv->stm_optional && priv->stm_iface == U8_MAX) ||
	    priv->keyboard_iface == U8_MAX)
		return false;

	return !priv->retention_capable || priv->touch_iface != U8_MAX;
}

static int apple_mtp_discover(struct apple_mtp_kbd_priv *priv,
			      struct input_config *input)
{
	ulong start = get_timer(0);
	ulong quiet = start;
	int ret;

	while (get_timer(start) < MTP_DISCOVERY_TIMEOUT_MS) {
		ret = apple_mtp_process_one(priv, input);
		if (ret > 0) {
			quiet = get_timer(0);
			continue;
		}
		if (ret < 0 && ret != -ETIMEDOUT)
			dev_warn(input->dev,
				 "discarded malformed MTP packet (%d)\n", ret);

		if (apple_mtp_discovery_complete(priv) &&
		    get_timer(quiet) >= MTP_DISCOVERY_QUIET_MS)
			return 0;

		apple_mtp_poll_rtkit(priv);
		udelay(50);
	}

	return -ETIMEDOUT;
}

static int apple_mtp_enable_interface(struct apple_mtp_kbd_priv *priv,
				      struct input_config *input, u8 iface)
{
	u8 command[] = { MTP_CMD_ENABLE_INTERFACE, iface };
	int ret;

	ret = apple_mtp_send_command(priv, input, 0, MTP_FEATURE_SET_REPORT,
				     command, sizeof(command), NULL, 0, NULL);
	if (ret)
		return ret;

	return apple_mtp_wait_ready(priv, input, iface);
}

static int apple_mtp_initialize_stm(struct apple_mtp_kbd_priv *priv,
				    struct input_config *input)
{
	u8 response[64];
	size_t response_size;
	u8 report;
	int ret;

	ret = apple_mtp_enable_interface(priv, input, priv->stm_iface);
	if (ret)
		return ret;

	report = MTP_STM_REPORT_ID;
	ret = apple_mtp_send_command(priv, input, priv->stm_iface,
				     MTP_FEATURE_GET_REPORT, &report,
				     sizeof(report), response, sizeof(response),
				     &response_size);
	if (ret || !response_size)
		return ret ?: -EPROTO;

	report = MTP_STM_REPORT_SERIAL;
	ret = apple_mtp_send_command(priv, input, priv->stm_iface,
				     MTP_FEATURE_GET_REPORT, &report,
				     sizeof(report), response, sizeof(response),
				     &response_size);
	if (ret || !response_size)
		return ret ?: -EPROTO;

	return 0;
}

static int apple_mtp_initialize_keyboard(struct apple_mtp_kbd_priv *priv,
					 struct input_config *input)
{
	int ret;

	/* T8140 announces "mtp", not "stm". Never send STM reports to it. */
	if (priv->stm_iface != U8_MAX) {
		ret = apple_mtp_initialize_stm(priv, input);
		if (ret)
			return ret;
	}

	ret = apple_mtp_enable_interface(priv, input, priv->keyboard_iface);
	if (ret)
		return ret;

	priv->keyboard_enabled = true;
	return 0;
}

static int apple_mtp_kbd_check(struct input_config *input)
{
	struct apple_mtp_kbd_priv *priv = dev_get_priv(input->dev);
	int ret;

	ret = apple_mtp_process_one(priv, input);
	if (ret == -ETIMEDOUT)
		return 0;
	if (ret < 0) {
		dev_warn(input->dev, "discarded malformed MTP packet (%d)\n", ret);
		return 0;
	}

	return input->fifo_in != input->fifo_out;
}

static int get_rtkit_helper(struct udevice *dev)
{
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	u32 phandle;
	int ret;

	ret = dev_read_u32(dev, "apple,helper-cpu", &phandle);
	if (ret)
		return ret;

	priv->helper_node = ofnode_get_by_phandle(phandle);
	return uclass_get_device_by_ofnode(UCLASS_MISC, priv->helper_node,
					   &priv->helper);
}

static int apple_mtp_kbd_probe(struct udevice *dev)
{
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	struct keyboard_priv *uc_priv = dev_get_uclass_priv(dev);
	struct stdio_dev *sdev = &uc_priv->sdev;
	struct input_config *input = &uc_priv->input;
	fdt_addr_t reg;
	int ret;

	reg = dev_read_addr_name(dev, "data");
	if (reg == FDT_ADDR_T_NONE) {
		dev_err(dev, "missing local FIFO data registers\n");
		return -EINVAL;
	}
	priv->local = (void *)reg;

	reg = dev_read_addr_name(dev, "rmt-data");
	if (reg == FDT_ADDR_T_NONE) {
		dev_err(dev, "missing remote FIFO data registers\n");
		return -EINVAL;
	}
	priv->rmt = (void *)reg;

	ret = dev_read_u32(dev, "apple,fifo-size", &priv->fifo_size);
	if (ret || priv->fifo_size < 64) {
		dev_err(dev, "invalid FIFO size\n");
		return ret ?: -EINVAL;
	}

	ret = get_rtkit_helper(dev);
	if (ret) {
		dev_err(dev, "failed to start MTP helper (%d)\n", ret);
		return ret;
	}
	priv->retention_capable =
		apple_rtkit_helper_can_retain(priv->helper);
	{
		int size;

		/* Preserve the input tree's boolean/u32 handoff representation. */
		priv->handoff_u32 =
			ofnode_get_property(priv->helper_node, MTP_INHERITED_PROP, &size) &&
			size == sizeof(u32);
	}

	priv->frame_capacity = priv->fifo_size +
			       sizeof(struct apple_mtp_header) + sizeof(u32);
	priv->frame = malloc(priv->frame_capacity);
	priv->init_data = malloc(priv->fifo_size);
	if (!priv->frame || !priv->init_data) {
		ret = -ENOMEM;
		goto err_free;
	}

	priv->stm_iface = U8_MAX;
	priv->keyboard_iface = U8_MAX;
	priv->touch_iface = U8_MAX;
	priv->stm_optional = dev_get_driver_data(dev);
	input->dev = dev;
	input->read_keys = apple_mtp_kbd_check;
	ret = input_add_tables(input, false);
	if (ret)
		goto err_free;

	ret = apple_mtp_discover(priv, input);
	if (ret) {
		dev_err(dev, "MTP interface discovery failed (%d)\n", ret);
		goto err_free;
	}
	ret = apple_mtp_initialize_keyboard(priv, input);
	if (ret) {
		dev_err(dev, "MTP keyboard initialization failed (%d)\n", ret);
		goto err_free;
	}

	if (priv->retention_capable) {
		/*
		 * M3 can quiesce MTP and let Linux create a fresh session.
		 * Retention-capable platforms keep this working session and all
		 * referenced buffers rather than restart firmware at this boundary.
		 */
		ret = apple_rtkit_helper_retain(priv->helper);
		if (ret)
			goto err_free;
		priv->retained = true;
	}

	strcpy(sdev->name, "mtpkbd");
	ret = input_stdio_register(sdev);
	if (ret)
		goto err_release;

	dev_info(dev, "MTP keyboard ready (keyboard %u%s%s)\n",
		 priv->keyboard_iface, priv->stm_iface == U8_MAX ? ", no STM" : "",
		 priv->retained ? ", retained handoff" : "");
	return 0;

err_release:
	if (priv->retained) {
		apple_rtkit_helper_release(priv->helper);
		priv->retained = false;
	}
err_free:
	free(priv->init_data);
	free(priv->frame);
	priv->init_data = NULL;
	priv->frame = NULL;
	return ret;
}

int apple_mtp_kbd_fixup_fdt(struct udevice *dev, void *fdt)
{
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	char path[MTP_FDT_PATH_SIZE];
	int ret, offset;

	if (!priv->retained)
		return 0;
	ret = apple_rtkit_helper_fixup_fdt(priv->helper, fdt);
	if (ret)
		return ret;
	ret = ofnode_get_path(priv->helper_node, path, sizeof(path));
	if (ret)
		return ret;
	offset = fdt_path_offset(fdt, path);
	if (offset < 0)
		return offset;

	/* EFI copies this tree before ExitBootServices replays the FIFO. */
	return priv->handoff_u32 ?
		fdt_setprop_u32(fdt, offset, MTP_INHERITED_PROP, 1) :
		fdt_setprop(fdt, offset, MTP_INHERITED_PROP, NULL, 0);
}

static int apple_mtp_kbd_remove(struct udevice *dev)
{
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	struct keyboard_priv *uc_priv = dev_get_uclass_priv(dev);
	struct input_config *input = &uc_priv->input;
	ulong start;
	u32 pending;
	int ret;

	if (!priv->retained && priv->helper) {
		ret = device_remove(priv->helper, DM_REMOVE_NORMAL);
		if (ret)
			return ret;
		priv->helper = NULL;
	}

	priv->shutting_down = true;
	start = get_timer(0);
	while (readl(priv->local + DATA_RX_COUNT) &&
	       get_timer(start) < MTP_DRAIN_TIMEOUT_MS) {
		ret = apple_mtp_process_one(priv, input);
		if (ret <= 0) {
			apple_mtp_poll_rtkit(priv);
			udelay(50);
		}
	}

	pending = readl(priv->local + DATA_RX_COUNT);
	if (pending) {
		u32 drain_size = min_t(u32, pending, priv->fifo_size);

		dockchannel_drain_bounded(priv, drain_size);
		if (readl(priv->local + DATA_RX_COUNT)) {
			dev_err(dev, "receive FIFO did not drain for OS handoff\n");
			return -ETIMEDOUT;
		}
	}

	ret = dockchannel_write(priv, priv->rmt, priv->init_data,
				priv->init_size);
	if (ret) {
		dev_err(dev, "discovery replay failed (%d)\n", ret);
		return ret;
	}

	dev_info(dev, "replayed %zu MTP discovery bytes%s\n",
		 priv->init_size,
		 priv->retained ? " and retained firmware for Linux" : "");
	free(priv->init_data);
	free(priv->frame);
	priv->init_data = NULL;
	priv->frame = NULL;
	return 0;
}

static const struct keyboard_ops apple_mtp_kbd_ops = {
};

static const struct udevice_id apple_mtp_kbd_of_match[] = {
	{ .compatible = "apple,t8140-dockchannel-hid", .data = 1 },
	{ .compatible = "apple,dockchannel-hid" },
	{ }
};

U_BOOT_DRIVER(apple_mtp_kbd) = {
	.name = "apple_mtp_kbd",
	.id = UCLASS_KEYBOARD,
	.of_match = apple_mtp_kbd_of_match,
	.probe = apple_mtp_kbd_probe,
	.remove = apple_mtp_kbd_remove,
	.priv_auto = sizeof(struct apple_mtp_kbd_priv),
	.ops = &apple_mtp_kbd_ops,
	.flags = DM_FLAG_OS_PREPARE,
};

/* Treat DockChannel as a simple bus; U-Boot polls its HID child. */

static const struct udevice_id dockchannel_bus_ids[] = {
	{ .compatible = "apple,dockchannel" },
	{ }
};

U_BOOT_DRIVER(dockchannel) = {
	.name = "dockchannel",
	.id = UCLASS_SIMPLE_BUS,
	.of_match = of_match_ptr(dockchannel_bus_ids),
};
