#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""Build a RAM-only J700 first-light chain from sibling cleanroom repos.

This is an experimental single-CPU Linux fixture, not an installer or full DT.
It does not contact hardware. Requires a previously built J700 U-Boot and
ARM64 EFI GRUB, and a statically linked AArch64 BusyBox supplied by the caller.
"""

import argparse
import gzip
from pathlib import Path
import struct
import subprocess
import hashlib


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--busybox', type=Path, required=True)
    parser.add_argument('--uboot', type=Path, required=True)
    parser.add_argument('--font', type=Path, default=Path('/usr/share/grub/unicode.pf2'))
    parser.add_argument('-j', type=int, default=8)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    root = repo.parent
    work = args.output_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    fixture = repo / 'doc/board/apple/j700-first-light'
    linux = root / 'linux-alpha'
    m1n1 = root / 'm1n1-alpha'
    grub = root / 'grub-alpha/build-arm64-efi'
    busybox = args.busybox.resolve()
    # gen_init_cpio uses whitespace-delimited paths.
    if any(c.isspace() for c in str(busybox) + str(work) + str(fixture)):
        parser.error('initramfs source paths must not contain whitespace')
    if not busybox.is_file():
        parser.error('BusyBox does not exist')
    initlist = work / 'initramfs.list'
    initlist.write_text(''.join(f'dir /{d} 755 0 0\n' for d in ('bin', 'dev', 'proc', 'sys', 'tmp')) +
                        'nod /dev/console 600 0 0 c 5 1\n'
                        'nod /dev/null 666 0 0 c 1 3\n' +
                        f'file /bin/busybox {busybox} 755 0 0\n' +
                        'slink /bin/sh /bin/busybox 777 0 0\n' +
                        f'file /init {fixture / "init"} 755 0 0\n')
    config = work / 'linux.config'
    config.write_text((fixture / 'linux.config').read_text() +
                      f'CONFIG_INITRAMFS_SOURCE="{initlist}"\n')
    make = ['make', '-C', linux, f'O={work / "linux-build"}',
            'ARCH=arm64', 'CROSS_COMPILE=aarch64-linux-gnu-']
    run(*make, f'KCONFIG_ALLCONFIG={config}', 'allnoconfig')
    enabled = (work / 'linux-build/.config').read_text().splitlines()
    for feature in ('INPUT_EVDEV', 'HID_APPLE', 'HID_DOCKCHANNEL',
                    'APPLE_DOCKCHANNEL', 'APPLE_DART', 'APPLE_MAILBOX',
                    'APPLE_RTKIT_HELPER', 'VT_CONSOLE', 'FB_SIMPLE'):
        if f'CONFIG_{feature}=y' not in enabled:
            raise ValueError(f'CONFIG_{feature} was disabled by Kconfig dependencies')
    run(*make, f'-j{args.j}', 'Image')
    run('make', '-C', m1n1, f'-j{args.j}', 'RELEASE=1', 'CHAINLOADING=0',
        'USE_CLANG=1', 'BUILDSTD=1')
    pre = work / 'boot.pre.dts'
    dtb = work / 'boot.dtb'
    run('aarch64-linux-gnu-gcc', '-E', '-nostdinc', '-undef', '-D__DTS__',
        '-x', 'assembler-with-cpp', '-I', repo, '-I', linux / 'include',
        fixture / 'boot.dts', '-o', pre)
    run('dtc', '-I', 'dts', '-O', 'dtb', '-o', dtb, pre)
    run(grub / 'grub-script-check', fixture / 'grub.cfg')
    modules = 'normal configfile echo sleep test efi_gop gfxterm font terminal minicmd linux'
    efi_path = work / 'grub-linux.efi'
    run(grub / 'grub-mkstandalone', '-O', 'arm64-efi', '-d', grub / 'grub-core',
        '--fonts=', '--locales=', '--themes=', f'--install-modules={modules}',
        f'--modules={modules}', '-o', efi_path,
        f'boot/grub/grub.cfg={fixture / "grub.cfg"}',
        f'boot/grub/fonts/unicode.pf2={args.font.resolve()}',
        f'boot/Image={work / "linux-build/arch/arm64/boot/Image"}')
    uboot = bytearray(args.uboot.read_bytes())
    efi = efi_path.read_bytes()
    if uboot[0x38:0x3c] != b'ARM\x64' or efi[:2] != b'MZ':
        raise ValueError('invalid U-Boot ARM64 Image or GRUB EFI image')
    offset = (max(len(uboot), struct.unpack_from('<Q', uboot, 0x10)[0]) + 4095) & ~4095
    if offset + len(efi) > 128 * 1024 * 1024:
        raise ValueError('combined image exceeds fixture limit')
    uboot.extend(bytes(offset - len(uboot)))
    uboot.extend(efi)
    struct.pack_into('<Q', uboot, 0x10, len(uboot))
    for name, cells in (
        ('u-boot,preloaded-efi-offset', ['0', hex(offset)]),
        ('u-boot,preloaded-efi-size', [hex(len(efi))]),
        ('u-boot,preloaded-efi-address', ['0', '0']),
    ):
        run('fdtput', '-t', 'x', dtb, '/chosen', name, *cells)
    payload = ((m1n1 / 'build/m1n1.bin').read_bytes() + dtb.read_bytes() +
               gzip.compress(uboot, mtime=0) + bytes(4))
    (work / 'linux-chain.bin').write_bytes(payload)
    for name, data in (('linux-chain.bin', payload), ('grub-linux.efi', efi)):
        print(f'{name}: {len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
