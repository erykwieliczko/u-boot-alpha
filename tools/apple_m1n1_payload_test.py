# SPDX-License-Identifier: GPL-2.0+ OR MIT
import gzip
import struct
import unittest

from apple_m1n1_payload import build_payload


class PayloadTests(unittest.TestCase):
    def setUp(self):
        self.stage2 = b'm1n1-stage2-test'
        self.dtb = struct.pack('>10I', 0xd00dfeed, 40, *([0] * 8))
        self.uboot = bytearray(64)
        self.uboot[0x38:0x3c] = b'ARM\x64'
        struct.pack_into('<Q', self.uboot, 0x10, 0x200000)

    def test_round_trip_and_determinism(self):
        image = build_payload(self.stage2, self.dtb, self.uboot)
        self.assertEqual(image, build_payload(self.stage2, self.dtb, self.uboot))
        prefix = self.stage2 + self.dtb
        self.assertEqual(image[:len(prefix)], prefix)
        self.assertEqual(image[-4:], bytes(4))
        self.assertEqual(gzip.decompress(image[len(prefix):-4]), self.uboot)

    def test_bad_inputs(self):
        inputs = [(b'', self.dtb, self.uboot),
                  (self.stage2, b'', self.uboot),
                  (self.stage2, self.dtb + b'x', self.uboot),
                  (self.stage2, self.dtb, b'not an Image')]
        for args in inputs:
            with self.subTest(args=args), self.assertRaises(ValueError):
                build_payload(*args)

    def test_bad_image_reservation(self):
        for size in (0, 63, 128 * 1024 * 1024 + 1):
            with self.subTest(size=size), self.assertRaises(ValueError):
                struct.pack_into('<Q', self.uboot, 0x10, size)
                build_payload(self.stage2, self.dtb, self.uboot)


if __name__ == '__main__':
    unittest.main()
