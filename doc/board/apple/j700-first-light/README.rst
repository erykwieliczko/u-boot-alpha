J700 Linux first light (experimental)
===================================

This fixture boots unmodified GRUB followed by a single-CPU Linux kernel and
embedded BusyBox. It is not a production platform DT, installer or replacement
for installed boot files. The sparse U-Boot tree is deliberately retained;
Linux storage, touchpad, networking, platform power management, KVM and PMU
drivers are not enabled. The built-in keyboard uses Linux DockChannel HID and
the Apple HID driver; physical UART remains an output-only early printk console.

Required sibling repositories are m1n1-alpha, linux-alpha and grub-alpha.
Build the J700 U-Boot configuration and ARM64 EFI GRUB first, as described in
``../j700.rst``. Supply a statically linked AArch64 BusyBox (do not use a host
x86 executable). From the workspace::

    python3 u-boot-alpha/tools/apple_j700_first_light.py \
        --output-dir /absolute/path/to/build-directory \
        --busybox /absolute/path/to/aarch64/busybox \
        --uboot /absolute/path/to/j700-build/u-boot-nodtb.bin -j12

The script builds an isolated Linux output directory, rebuilds stage2, compiles
``boot.dts`` against the clean U-Boot tree and Linux bindings, and embeds the
Image/initramfs and stock Unicode font in GRUB. It reports sizes and SHA256 for
``linux-chain.bin`` and ``grub-linux.efi``. It never contacts a target.

The package uses m1n1's existing gzip ARM64 Image and preloaded-EFI descriptor
ABI. The J700 console config does not auto-execute that descriptor. After a
target-verified RAM chainload, inspect its current value and the load buffer::

    fdt addr ${fdtcontroladdr}
    fdt print /chosen u-boot,preloaded-efi-address
    fdt print /chosen u-boot,preloaded-efi-size
    printenv loadaddr

Copy the EFI image from that live address into the reserved load buffer with
``cp.b``. Confirm its actual size fits within the 64-MiB buffer, and compare
``hash sha256`` against the builder's digest before executing::

    bootefi ${loadaddr}:<actual-EFI-size-in-hex> ${fdtcontroladdr}

Do not reuse physical addresses from a previous boot. The boot CPU node is
filled from MPIDR_EL1 by stage2, not from the ADT logical CPU number. The
first-light m1n1 path keeps other CPUs parked and provides no SMP release ABI.
The timer's four standard named interrupt roles remain in the fixture, while
the Linux T8140 irqdomain rejects unsupported guest/PMU roles. Host timers
work at EL2 VHE. Keep both ``maxcpus=1 nr_cpus=1`` and the one-CPU DT.

Fast upload from a Linux controller
----------------------------------

The hardware-proven fast route is Apple's native DebugUSB/KIS, not a new DWC3
handoff. The controller needs ``kisd`` and its normal m1n1 Python client. Neo
enumerates as 05ac:1881. Run exactly one bridge with::

    kisd --base 0x348000000

Check for an existing bridge first. An older bridge can own the USB interface
while a second one creates a new, nonfunctional ``/dev/m1n1`` link. Verify the
PTY belonging to the running bridge; its device number is not persistent.
This parameter selects the KIS bridge protocol address, not a memory-write
instruction. Use the controller's VDM tool to reboot the attached target into
DebugUSB mode, then run the normal target-verified chainloader on that PTY.
Use ``--no-wait -r -c``: this handoff ends in U-Boot, not another proxy.

After upload, use the VDM ``serial`` operation **without another reboot** to
return to physical UART. This transition was tested on J700 with a Fedora M1
controller. U-Boot accepts commands at inherited 115200 baud. Compare the
loaded EFI hash, then boot. No disk write or installed stage1 replacement is
needed. KIS bulk transfers are not limited by the PTY's nominal baud setting.

Physical UART experiments are separate. A corrected host baud callback and
transmit drain allowed switching rates, but a long 500000-baud readback failed
its checksum. Do not use a rate merely because a short proxy request works.
The 1.5-Mbaud experiment also lost a byte from the baud-change reply. In
contrast, the final 5209749-byte chain uploaded and handed off over KIS in
11.18 seconds, including proxy setup and ADT handling. Its 9990144-byte EFI
image matched the host SHA256 after relocation into U-Boot's reserved buffer.

Acceptance and limitations
--------------------------

The initial successful hardware test on 2026-09-06 logged:

* AIC3: 1504/4096 IRQs, one active die, two host FIQs, zero vIPIs.
* Architectural physical timer at 1000 MHz (not the 24 MHz UART reference).
* simplefb at 2408x1506x32, padded stride 9664, framebuffer console enabled.
* ``NEO_INITRAMFS: Linux reached BusyBox userspace.``
* ``NEO_INITRAMFS: two-second userspace timer completed.``

The final build reproduced those milestones after a second USB upload. Its
16x32 font produced a 150x47 text console; userspace was reached at 2.79 seconds
and the timer marker at 4.81 seconds. KVM and performance-event consumers were
disabled. The host Python suite passed 10 tests (6 skipped); the existing
payload packaging suite passed 3 tests, and kernel checkpatch reported no
errors or warnings for the changes.

The preceding failure was an undefined MSR to ``S3_5_C15_C1_3`` at
``aic_init_cpu+0x50``. Its replacement guest-routing mechanism is unknown; the
first-light driver does not claim guest timers, PMU or IPIs work. Unexpected
unsupported FIQs fail explicitly instead of returning into an interrupt storm.
Existing Apple platforms retain their earlier capability selection.

Firmware reservations are preserved. The current sparse handoff still emits
overlap diagnostics for rounded MTP SRAM segments and the encompassing
chainload reservation. EFI also exposes these firmware ranges as reserved
memory. Audit and normalize this map before production or broader peripheral
bring-up; first-light success does not validate firmware ownership transfer.

No native NVMe/Linux disk takeover, SMP/IPI, KVM/guest timer, PMU, suspend or
shutdown qualification is implied by this fixture.

Linux keyboard extension
------------------------

The input extension describes MTP mailbox IRQs 1152..1155 in semantic order,
DockChannel parent IRQ 1137 and local TX/RX IRQs 2/3, and DART SID 0. The
``keyboard`` alias now addresses the HID keyboard child so stage2 can pass the
actual layout. ``apple,no-stm`` makes identity available without waiting for
an STM interface which this firmware does not expose. Product, version and
serial remain unknown rather than borrowing another machine's identity.
The firmware's HID descriptor, not a fixed boot-keyboard packet decoder,
controls Linux report parsing.

J700 currently retains the working U-Boot MTP session. All AP-allocated RTKit
buffers are reserved before EFI copies the FDT. At ExitBootServices the input
consumer is removed and discovery is replayed, without stopping MTP. The
preallocated 32-bit ``linux-enablement-mac,rtkit-inherited`` word changes from
zero to one without moving flat-DT device offsets. Linux also continues to
accept the existing empty boolean marker used by J713.

The DART's preallocated ``linux-enablement-mac,retained-bypass`` word changes
from zero to one for the already-validated SID 0 bypass session. Linux verifies
that SID 0 really is in bypass with no valid TTBR, preserves it without reset,
and attaches an identity domain. This is not translated DMA isolation. Native
translated takeover, additional streams, domain replacement and suspend are
not supported by this retained profile. Other Apple DARTs retain their existing
initialization. The Linux inherited helper currently consumes no RTKit IPC;
this is FIFO ownership transfer, not a complete active RTKit session import.

Stage2 must describe MTP firmware using the ADT **remap** addresses, not the
ASC's internal firmware virtual addresses. An earlier tree wrongly requested
translated IOMMU mappings using those internal VAs. Linux correctly rejected
combining that description with the live identity domain.

On 2026-09-06 the corrected retained chain reached ``/init`` and registered
``Apple MTP keyboard`` through the Apple HID driver. No firmware crash, DART
fault or kernel oops was logged in that boot. The operator subsequently
confirmed that the physical keyboard works in the Linux shell. Individual
modifier and navigation keys were not separately reported. For future tests,
``echo KEYBOARD_OK > /dev/kmsg`` provides a UART-visible confirmation.

The final follow-up guards against replacing or suspending the retained DART
domain and clears the handoff word on U-Boot probe rollback. Those defensive
changes were build-tested after the successful hardware run, not reboot-tested.
U-Boot M1 and J713 configurations also rebuilt successfully; that is compile
coverage, not a hardware regression test on those machines.

Stopped-handoff experiments, including reserved OSLog import and endpoint and
power-request comparisons, failed before the final remap correction. They are
not included in this profile. This does **not** establish that Neo firmware
fundamentally cannot restart: a future native takeover must retest with the
correct DMA description rather than assume that conclusion.

The kernel also handles the no-IPI first-light profile through the existing
timer-tick IRQ-work fallback. A separate two-core ARM64/GICv3 QEMU regression
test exercised normal IPIs, including IRQ work; it was not an emulation or
keyboard test of Neo. Existing MTP frame/Neo report unit tests passed (4), and
m1n1 Python tests passed (10, with 6 toolchain-dependent skips).
