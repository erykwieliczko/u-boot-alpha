#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""Host-only envelope checks: no emulated or physical input device needed."""
import struct
from pathlib import Path
import tempfile
import unittest

from apple_j700_first_light import merge_initramfs_root, validate_touchpad_firmware


def envelope(magic=b'HIDF', version=1, header=32, length=4, iface=1):
    return struct.pack('<4sIIII12x', magic, version, header, length, iface) + bytes(4)


class FirmwareTests(unittest.TestCase):
    def test_valid(self):
        validate_touchpad_firmware(envelope())

    def test_truncation(self):
        for size in range(36):
            with self.subTest(size=size), self.assertRaises(ValueError):
                validate_touchpad_firmware(envelope()[:size])

    def test_wrong_format(self):
        for data in (envelope(magic=b'Z2FW'), envelope(version=2)):
            with self.assertRaises(ValueError):
                validate_touchpad_firmware(data)

    def test_bounds(self):
        for data in (envelope(header=20), envelope(header=0xffffffff),
                     envelope(length=0), envelope(length=0xffffffff),
                     envelope(iface=4), envelope(iface=0xffffffff),
                     envelope() + b'\0'):
            with self.assertRaises(ValueError):
                validate_touchpad_firmware(data)


class RootMergeTests(unittest.TestCase):
    def test_shared_directories_and_libraries(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'lib64').mkdir()
            library = root / 'lib64/libc.so.6'
            library.write_bytes(b'same library')
            manifest = f'dir /lib64 755 0 0\nfile /lib64/libc.so.6 {library} 755 0 0\n'
            self.assertEqual(merge_initramfs_root(manifest, root), manifest)
            (root / 'lib64/new.so').write_bytes(b'new library')
            self.assertIn('file /lib64/new.so ', merge_initramfs_root(manifest, root))

    def test_reject_conflicts_and_symlinks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'lib64').mkdir()
            library = root / 'lib64/libc.so.6'
            library.write_bytes(b'library')
            with self.assertRaises(ValueError):
                merge_initramfs_root('slink /lib64/libc.so.6 /elsewhere 777 0 0\n', root)
            (root / 'bad').symlink_to(library)
            with self.assertRaises(ValueError):
                merge_initramfs_root('', root)


if __name__ == '__main__':
    unittest.main()
