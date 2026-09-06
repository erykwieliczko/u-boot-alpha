#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""Send a RAM image to an existing U-Boot prompt using standard YMODEM.

Requires pyserial and lrzsz's sb. This does not reboot or install anything.
Stop other UART readers and keyboard input before starting. Verify the image
hash in U-Boot before executing it; successful transfer alone is insufficient.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--address', required=True, type=lambda s: int(s, 16),
                        help='hex address in the current reserved load buffer')
    parser.add_argument('--timeout', type=int, default=480)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    if args.address <= 0 or args.baud <= 0 or args.timeout <= 0:
        parser.error('address, baud and timeout must be positive')
    if not args.image.is_file() or not args.image.stat().st_size:
        parser.error('image must be a nonempty regular file')
    sender = shutil.which('sb')
    if not sender:
        parser.error('install lrzsz (sb) on the UART controller')

    import serial

    with serial.Serial(args.device, args.baud, timeout=0.1,
                       exclusive=True) as port:
        def receive(marker, timeout):
            result = bytearray()
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                data = port.read(1)
                result.extend(data)
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                if marker in result:
                    return result
            raise TimeoutError(f'UART did not return {marker!r}')

        port.write(b'\x03')
        receive(b'=> ', 5)
        port.write(f'loady {args.address:x}\r'.encode('ascii'))
        receive(b'bps...', 10)
        # pyserial opens O_NONBLOCK; sb expects ordinary blocking tty reads.
        os.set_blocking(port.fileno(), True)
        try:
            result = subprocess.run(
                [sender, '--ymodem', '--binary', str(args.image.resolve())],
                stdin=port.fileno(), stdout=port.fileno(),
                timeout=args.timeout, check=False,
            )
        finally:
            os.set_blocking(port.fileno(), False)
        if result.returncode:
            raise RuntimeError(f'YMODEM sender failed: {result.returncode}')
        result = receive(b'=> ', 20)
        if b'aborted' in result or b'Total Size' not in result:
            raise RuntimeError('U-Boot did not confirm the completed transfer')


if __name__ == '__main__':
    main()
