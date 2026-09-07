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


def validate_touchpad_firmware(data):
    """Check the installer HIDF envelope, without interpreting firmware code."""
    if len(data) < 32:
        raise ValueError('touchpad firmware has a truncated HIDF header')
    magic, version, header, length, iface = struct.unpack_from('<4sIIII', data)
    if (magic != b'HIDF' or version != 1 or header < 32 or
            header + length != len(data) or iface >= length):
        raise ValueError('invalid HIDF touchpad firmware')


def merge_initramfs_root(manifest, root):
    """Merge dereferenced userspace files; refuse conflicting shared libraries."""
    entries = {line.split()[1]: line.split() for line in manifest.splitlines()}
    extra = []
    for path in sorted(root.rglob('*')):
        if any(c.isspace() for c in str(path)) or path.is_symlink():
            raise ValueError('root must contain dereferenced paths without whitespace')
        target = '/' + path.relative_to(root).as_posix()
        if target in entries:
            old = entries[target]
            if path.is_dir() and old[0] == 'dir':
                continue
            if path.is_file() and old[0] == 'file' and Path(old[2]).read_bytes() == path.read_bytes():
                continue
            raise ValueError(f'conflicting initramfs path: {target}')
        if path.is_dir():
            extra.append(f'dir {target} 755 0 0\n')
        elif path.is_file():
            mode = '755' if path.stat().st_mode & 0o111 else '644'
            extra.append(f'file {target} {path} {mode} 0 0\n')
        else:
            raise ValueError(f'unsupported initramfs file: {path}')
    return manifest + ''.join(extra)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--busybox', type=Path, required=True)
    parser.add_argument('--uboot', type=Path, required=True)
    parser.add_argument('--touchpad-firmware', type=Path,
                        help='Optional J700 HIDF firmware; enable the touchpad test fixture')
    parser.add_argument('--usb2', action='store_true',
                        help='Enable experimental J700 fixed-hub USB2 host bring-up')
    parser.add_argument('--usb2-front', action='store_true',
                        help='With --usb2, service front HPM1; rear debug HPM stays unbound')
    parser.add_argument('--usb-network-root', type=Path,
                        help='Optional AArch64 iw/ip/timeout, libraries and firmware root')
    parser.add_argument('--power-root', type=Path,
                        help='Optional AArch64 UPower/D-Bus/udev root; enable SMC battery')
    parser.add_argument('--font', type=Path, default=Path('/usr/share/grub/unicode.pf2'))
    parser.add_argument('-j', type=int, default=8)
    args = parser.parse_args()
    if args.usb2_front and not args.usb2:
        parser.error('--usb2-front requires --usb2')
    if args.usb_network_root and not args.usb2:
        parser.error('--usb-network-root requires --usb2')
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
    touch_files = ''
    touch_config = ''
    usb_files = ''
    usb_config = ''
    if args.usb2:
        mouse = work / 'usb-mouse-events'
        run('aarch64-linux-gnu-gcc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
            root / 'linux-enablement-mac-alpha/tools/usb-mouse-events.c', '-o', mouse)
        hub = work / 'usb-hub-status'
        run('aarch64-linux-gnu-gcc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
            root / 'linux-enablement-mac-alpha/tools/usb-hub-status.c', '-o', hub)
        usb_files = (f'file /bin/usb-status {fixture / "usb-status"} 755 0 0\n' +
                     f'file /bin/usb-mouse-events {mouse} 755 0 0\n' +
                     f'file /bin/usb-hub-status {hub} 755 0 0\n')
        usb_config = (fixture / 'usb2.config').read_text()
        if args.usb2_front:
            usb_config += 'CONFIG_SPMI=y\nCONFIG_SPMI_APPLE=y\nCONFIG_TYPEC_SN201202X=y\n'
    if args.touchpad_firmware:
        firmware = args.touchpad_firmware.resolve()
        if any(c.isspace() for c in str(firmware)):
            parser.error('firmware path must not contain whitespace')
        data = firmware.read_bytes()
        try:
            validate_touchpad_firmware(data)
        except ValueError as error:
            parser.error(str(error))
        viewer = work / 'touchpad-events'
        run('aarch64-linux-gnu-gcc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
            root / 'linux-enablement-mac-alpha/tools/touchpad-events.c', '-o', viewer)
        demo = work / 'touchpad-demo'
        run('aarch64-linux-gnu-gcc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
            root / 'linux-enablement-mac-alpha/tools/touchpad-demo.c', '-o', demo)
        touch_files = (''.join(f'dir /{d} 755 0 0\n' for d in
                              ('lib', 'lib/firmware', 'lib/firmware/apple')) +
                       f'file /lib/firmware/apple/tpmtfw-j700.bin {firmware} 644 0 0\n' +
                       f'file /bin/touchpad-events {viewer} 755 0 0\n' +
                       f'file /bin/touchpad-demo {demo} 755 0 0\n')
        touch_config = ('CONFIG_HID_MAGICMOUSE=y\nCONFIG_MFD_MACSMC=y\n'
                        'CONFIG_GPIOLIB=y\nCONFIG_GPIO_MACSMC=y\n'
                        'CONFIG_FB_DEVICE=y\n')
    initlist = work / 'initramfs.list'
    if args.usb_network_root:
        network = args.usb_network_root.resolve()
        for required in ('bin/iw', 'bin/ip', 'bin/timeout',
                         'lib/firmware/rtlwifi/rtl8188eufw.bin'):
            if not (network / required).is_file():
                parser.error(f'network root lacks {required}')
        for path in sorted(network.rglob('*')):
            relative = path.relative_to(network).as_posix()
            if any(c.isspace() for c in str(path)):
                parser.error('network root paths must not contain whitespace')
            if path.is_symlink():
                parser.error('provide dereferenced network files, not symlinks')
            if relative == 'bin' or (args.touchpad_firmware and relative in ('lib', 'lib/firmware')):
                continue
            if path.is_dir():
                usb_files += f'dir /{relative} 755 0 0\n'
            elif path.is_file():
                mode = '755' if relative.startswith(('bin/', 'lib/', 'lib64/')) else '644'
                usb_files += f'file /{relative} {path} {mode} 0 0\n'
        usb_files += f'file /bin/usb-network-test {fixture / "usb-network-test"} 755 0 0\n'
        usb_config += (fixture / 'usb-network.config').read_text()
    initlist.write_text(''.join(f'dir /{d} 755 0 0\n' for d in ('bin', 'dev', 'proc', 'sys', 'tmp')) +
                        'nod /dev/console 600 0 0 c 5 1\n'
                        'nod /dev/null 666 0 0 c 1 3\n' +
                        f'file /bin/busybox {busybox} 755 0 0\n' +
                        'slink /bin/sh /bin/busybox 777 0 0\n' +
                        f'file /init {fixture / "init"} 755 0 0\n' + touch_files + usb_files)
    if args.power_root:
        power_root = args.power_root.resolve()
        for required in ('usr/bin/upower', 'usr/libexec/upowerd',
                         'usr/bin/dbus-daemon', 'usr/bin/dbus-uuidgen',
                         'usr/bin/udevadm', 'usr/lib/systemd/systemd-udevd',
                         'etc/UPower/UPower.conf',
                         'usr/share/dbus-1/system.conf',
                         'usr/share/dbus-1/system.d/org.freedesktop.UPower.conf'):
            if not (power_root / required).is_file():
                parser.error(f'power root lacks {required}')
        manifest = initlist.read_text().replace('dir /tmp 755 0 0\n', 'dir /tmp 1777 0 0\n')
        initlist.write_text(merge_initramfs_root(manifest, power_root) +
                           f'file /bin/power-test {fixture / "power-test"} 755 0 0\n' +
                           f'file /etc/passwd {fixture / "power-passwd"} 644 0 0\n' +
                           f'file /etc/group {fixture / "power-group"} 644 0 0\n')
        usb_config += (fixture / 'power.config').read_text()
    config = work / 'linux.config'
    config.write_text((fixture / 'linux.config').read_text() +
                      touch_config + usb_config + f'CONFIG_INITRAMFS_SOURCE="{initlist}"\n')
    make = ['make', '-C', linux, f'O={work / "linux-build"}',
            'ARCH=arm64', 'CROSS_COMPILE=aarch64-linux-gnu-']
    run(*make, f'KCONFIG_ALLCONFIG={config}', 'allnoconfig')
    enabled = (work / 'linux-build/.config').read_text().splitlines()
    for feature in ('INPUT_EVDEV', 'HID_APPLE', 'HID_DOCKCHANNEL',
                    'APPLE_DOCKCHANNEL', 'APPLE_DART', 'APPLE_MAILBOX',
                    'APPLE_RTKIT_HELPER', 'VT_CONSOLE', 'FB_SIMPLE'):
        if f'CONFIG_{feature}=y' not in enabled:
            raise ValueError(f'CONFIG_{feature} was disabled by Kconfig dependencies')
    if args.touchpad_firmware:
        for feature in ('HID_MAGICMOUSE', 'MFD_MACSMC', 'GPIO_MACSMC', 'FW_LOADER',
                        'FB_DEVICE'):
            if f'CONFIG_{feature}=y' not in enabled:
                raise ValueError(f'CONFIG_{feature} was disabled by Kconfig dependencies')
    if args.usb2:
        for feature in ('USB', 'USB_XHCI_HCD', 'USB_DWC3_APPLE', 'USB_DWC3_HOST',
                        'USB_HID', 'PHY_APPLE_ATC', 'PHY_APPLE_J700_REPEATER',
                        'I2C_APPLE', 'PINCTRL_APPLE_GPIO', 'APPLE_PMGR_PWRSTATE'):
            if f'CONFIG_{feature}=y' not in enabled:
                raise ValueError(f'CONFIG_{feature} was disabled by Kconfig dependencies')
    if args.usb2_front:
        for feature in ('SPMI_APPLE', 'TYPEC_SN201202X'):
            if f'CONFIG_{feature}=y' not in enabled:
                raise ValueError(f'CONFIG_{feature} was disabled by Kconfig dependencies')
    if args.usb_network_root:
        for feature in ('NET', 'INET', 'CFG80211', 'MAC80211', 'RTL8XXXU', 'USB_RTL8152'):
            if f'CONFIG_{feature}=y' not in enabled:
                raise ValueError(f'CONFIG_{feature} was disabled by Kconfig dependencies')
    if args.power_root:
        for feature in ('MACSMC_POWER', 'MFD_MACSMC', 'POWER_SUPPLY', 'UNIX',
                        'INOTIFY_USER', 'FILE_LOCKING'):
            if f'CONFIG_{feature}=y' not in enabled:
                raise ValueError(f'CONFIG_{feature} was disabled by Kconfig dependencies')
    run(*make, f'-j{args.j}', 'Image')
    run('make', '-C', m1n1, f'-j{args.j}', 'RELEASE=1', 'CHAINLOADING=0',
        'USE_CLANG=1', 'BUILDSTD=1')
    pre = work / 'boot.pre.dts'
    dtb = work / 'boot.dtb'
    run('aarch64-linux-gnu-gcc', '-E', '-nostdinc', '-undef', '-D__DTS__',
        *(['-DNEO_TOUCHPAD'] if args.touchpad_firmware else []),
        *(['-DNEO_SMC'] if args.power_root else []),
        *(['-DNEO_USB2'] if args.usb2 else []),
        *(['-DNEO_USB2_FRONT'] if args.usb2_front else []),
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
