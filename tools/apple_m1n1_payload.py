#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""Package raw m1n1 stage2, its runtime DTB, and a gzip ARM64 U-Boot Image."""

import argparse
import gzip
from pathlib import Path
import struct


def build_payload(stage2, dtb, uboot):
    """The gzip payload lets m1n1 allocate U-Boot at its normal 2 MiB alignment."""
    if not stage2:
        raise ValueError('empty m1n1 stage2')
    if len(dtb) < 40 or struct.unpack_from('>I', dtb)[0] != 0xd00dfeed:
        raise ValueError('invalid FDT header')
    if struct.unpack_from('>I', dtb, 4)[0] != len(dtb):
        raise ValueError('FDT total size does not match file length')
    if len(uboot) < 64 or uboot[0x38:0x3c] != b'ARM\x64':
        raise ValueError('U-Boot is not a raw ARM64 Image')
    image_size = struct.unpack_from('<Q', uboot, 0x10)[0]
    if not len(uboot) <= image_size <= 128 * 1024 * 1024:
        raise ValueError('invalid ARM64 Image allocation size')
    return stage2 + dtb + gzip.compress(uboot, mtime=0) + bytes(4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage2', type=Path, required=True)
    parser.add_argument('--dtb', type=Path, required=True)
    parser.add_argument('--uboot', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build_payload(args.stage2.read_bytes(), args.dtb.read_bytes(),
                              args.uboot.read_bytes())
    except ValueError as error:
        parser.error(str(error))
    args.output.write_bytes(image)
    print(f'{args.output}: {len(image)} bytes')


if __name__ == '__main__':
    main()
