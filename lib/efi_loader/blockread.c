// SPDX-License-Identifier: GPL-2.0+
/* Read-only EFI Block I/O smoke test. Never calls WriteBlocks or Reset. */
#include <efi_api.h>

static const efi_guid_t block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static struct efi_simple_text_output_protocol *out;

static void number(u64 value)
{
	u16 text[19] = u"0x0000000000000000";
	int i;

	for (i = 17; i >= 2; i--, value >>= 4)
		text[i] = u"0123456789abcdef"[value & 15];
	out->output_string(out, text);
}

efi_status_t EFIAPI efi_main(efi_handle_t image,
			   struct efi_system_table *system)
{
	struct efi_boot_services *bs = system->boottime;
	efi_handle_t *handles;
	efi_uintn_t count, i;
	efi_status_t ret;
	unsigned int tested = 0, failed = 0;

	out = system->con_out;
	out->output_string(out, u"UEFI Block I/O read-only test\r\n");
	ret = bs->locate_handle_buffer(BY_PROTOCOL, &block_io_guid, NULL,
				      &count, &handles);
	if (ret != EFI_SUCCESS)
		return ret;

	for (i = 0; i < count; i++) {
		struct efi_block_io *io;
		struct efi_block_io_media *m;
		efi_uintn_t align, size;
		void *allocation, *first, *second;
		u32 crc;
		int sample;

		ret = bs->handle_protocol(handles[i], &block_io_guid, (void **)&io);
		if (ret != EFI_SUCCESS) {
			failed++;
			continue;
		}
		m = io->media;
		if (!m->media_present || m->logical_partition || m->removable_media)
			continue;
		tested++;
		out->output_string(out, u"Media ");
		number(m->media_id);
		out->output_string(out, u" last LBA ");
		number(m->last_block);
		out->output_string(out, u" block size ");
		number(m->block_size);
		out->output_string(out, u"\r\n");
		align = m->io_align > 1 ? m->io_align : 1;
		size = m->block_size;
		if (!size || size > 65536 || align > 65536 || (align & (align - 1))) {
			failed++;
			continue;
		}
		ret = bs->allocate_pool(EFI_LOADER_DATA, 2 * (size + align), &allocation);
		if (ret != EFI_SUCCESS) {
			failed++;
			continue;
		}
		first = (void *)(((efi_uintn_t)allocation + align - 1) & ~(align - 1));
		second = (void *)(((efi_uintn_t)first + size + align - 1) & ~(align - 1));
		for (sample = 0; sample < 2; sample++) {
			u64 lba = sample ? m->last_block : 0;

			memset(first, 0xa5, size);
			memset(second, 0x5a, size);
			ret = io->read_blocks(io, m->media_id, lba, size, first);
			if (ret == EFI_SUCCESS)
				ret = io->read_blocks(io, m->media_id, lba, size, second);
			if (ret != EFI_SUCCESS || memcmp(first, second, size)) {
				out->output_string(out, u"READ/COMPARE FAILED\r\n");
				failed++;
				break;
			}
			ret = bs->calculate_crc32(first, size, &crc);
			if (ret != EFI_SUCCESS) {
				failed++;
				break;
			}
			out->output_string(out, u"  PASS LBA ");
			number(lba);
			out->output_string(out, u" CRC32 ");
			number(crc);
			out->output_string(out, u"\r\n");
		}
		bs->free_pool(allocation);
	}
	bs->free_pool(handles);
	if (!tested || failed) {
		out->output_string(out, u"BLOCK READ TEST FAILED\r\n");
		return EFI_DEVICE_ERROR;
	}
	out->output_string(out, u"BLOCK READ TEST PASSED (no disk writes)\r\n");
	return EFI_SUCCESS;
}
