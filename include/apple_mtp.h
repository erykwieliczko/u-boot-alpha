/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
#ifndef __APPLE_MTP_H
#define __APPLE_MTP_H

#include <asm/unaligned.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#define APPLE_MTP_CHANNEL_COMMAND	0x11
#define APPLE_MTP_CHANNEL_REPORT		0x12
#define APPLE_MTP_EVENT_INIT		0xf0
#define APPLE_MTP_EVENT_READY		0xf1
#define APPLE_MTP_MAX_NAME		16

struct apple_mtp_header {
	u8 hdr_len;
	u8 channel;
	__le16 length;
	u8 seq;
	u8 iface;
	__le16 pad;
} __packed;

struct apple_mtp_subheader {
	u8 flags;
	u8 reserved;
	__le16 length;
	__le32 retcode;
} __packed;

struct apple_mtp_init_header {
	u8 type;
	u8 reserved[2];
	u8 iface;
	char name[APPLE_MTP_MAX_NAME];
	u8 more_packets;
	u8 pad;
} __packed;

static inline bool apple_mtp_keyboard_length_valid(size_t body_size, size_t payload_size)
{
	/* Ten HID bytes, with two bytes of transport alignment padding.
	 * Firmware may include that padding in the subheader length (M4)
	 * or report only the HID length (J700). Neither permits a short report.
	 */
	return body_size == sizeof(struct apple_mtp_subheader) + 12 &&
		(payload_size == 10 || payload_size == 12);
}

static inline u32 apple_mtp_checksum(const void *data, size_t size)
{
	const u8 *p = data;
	u32 sum = 0;

	while (size >= sizeof(u32)) {
		sum += get_unaligned_le32(p);
		p += sizeof(u32);
		size -= sizeof(u32);
	}

	return sum;
}

static inline int apple_mtp_validate_frame(const void *frame, size_t size)
{
	const struct apple_mtp_header *hdr = frame;
	u16 body_size;

	if (size < sizeof(*hdr) + sizeof(u32))
		return -EMSGSIZE;
	if (hdr->hdr_len != sizeof(*hdr))
		return -EPROTO;

	body_size = le16_to_cpu(hdr->length);
	if (body_size % sizeof(u32) ||
	    size != sizeof(*hdr) + body_size + sizeof(u32))
		return -EMSGSIZE;
	if (apple_mtp_checksum(frame, size) != ~0U)
		return -EBADMSG;

	return 0;
}

static inline int
apple_mtp_init_name(const void *payload, size_t size, u8 *iface,
		    char name[APPLE_MTP_MAX_NAME + 1], bool *more_packets)
{
	const struct apple_mtp_init_header *init = payload;

	if (size < sizeof(*init))
		return -EMSGSIZE;
	if (init->type != APPLE_MTP_EVENT_INIT)
		return -EPROTO;
	if (!init->name[0])
		return -EINVAL;

	memcpy(name, init->name, APPLE_MTP_MAX_NAME);
	name[APPLE_MTP_MAX_NAME] = '\0';
	*iface = init->iface;
	*more_packets = !!init->more_packets;

	return 0;
}

struct udevice;
int apple_mtp_kbd_fixup_fdt(struct udevice *dev, void *fdt);

#endif /* __APPLE_MTP_H */
