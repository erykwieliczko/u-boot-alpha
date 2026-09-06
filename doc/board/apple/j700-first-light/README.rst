J700 Linux first light (experimental)
===================================

This fixture boots unmodified GRUB followed by a single-CPU Linux kernel and
embedded BusyBox. It is not a production platform DT, installer or replacement
for installed boot files. The sparse U-Boot tree is deliberately retained;
Linux storage, keyboard, touchpad, networking, power management, KVM and PMU
drivers are not enabled. A visible shell is not yet an interactive Linux
console: the inherited UART implementation here is an early printk console.

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

No native NVMe/Linux disk takeover, physical IRQ device, SMP/IPI, KVM/guest
timer, PMU, suspend or shutdown qualification is implied by this fixture.
