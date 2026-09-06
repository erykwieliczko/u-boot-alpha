J700 Linux first light (experimental)
===================================

This fixture boots unmodified GRUB followed by a single-CPU Linux kernel and
embedded BusyBox. It is not a production platform DT, installer or replacement
for installed boot files. The sparse U-Boot tree is deliberately retained;
Linux storage, networking, platform power management, KVM and PMU
drivers are not enabled. The built-in keyboard uses Linux DockChannel HID and
the Apple HID driver; an optional firmware argument enables the touchpad test.
Physical UART remains an output-only early printk console.

Optional USB2 fixture
---------------------

``--usb2`` adds the shared T8140 DWC3 host, two translating DARTs, T8130
USB2/dummy-PIPE profile and an explicitly ordered J700 I2C repeater supplier.
``--usb2-front`` additionally services HPM1 (SPMI USID 0xa) using the existing
SN201202x driver. The rear HPM0/debug connector remains unbound. Front input
index 1 is preserved; it never selects the rear SuperSpeed mux or tears down
the shared host on disconnect. A real NONE-to-HOST notification synchronizes
the shared USB2 PHY once. This can disconnect/re-enumerate the onboard hub.

On 2026-09-07 the HPM-unbound profile enumerated only the onboard 2109:2122
hub. Both downstream ports reported power but no connection. The front-HPM
profile then enumerated the external 05e3:0610 hub, 258a:0017 HID mouse,
0bda:8179 Wi-Fi adapter and the hub's 0bda:8153 Ethernet adapter. UART and
built-in input survived. Enumeration alone is not network or mouse-event
acceptance. General cold boot, suspend/resume, rear-port use and simultaneous
two-port hotplug remain unqualified.

The following networking boot loaded RTL8188EU firmware revision 28.0,
registered ``wlan0`` and ``eth0``, and successfully completed a passive
``iw`` scan with three BSS results. Association, DHCP, packet throughput and
Ethernet carrier were not tested. The USB mouse's evdev monitor opened the
SINOWEALTH pointer successfully; movement/click acceptance still requires
the user's physical input.

The fixture includes ``/bin/usb-hub-status`` (standard GET_DESCRIPTOR and
GET_STATUS only), a bounded sysfs inventory, and ``/bin/usb-mouse-events``
(ordinary evdev, no exclusive grab). The mouse probe also reports to printk.

``--usb-network-root /absolute/root`` optionally embeds AArch64 ``bin/iw``,
``bin/ip``, ``bin/timeout``, their matching ELF interpreter/shared libraries,
``lib/firmware/rtlwifi/rtl8188eufw.bin`` and regulatory data. Supply ordinary
files, not symlinks, and no credentials. It enables stock rtl8xxxu/mac80211
and r8152 support. The RTL8153 firmware variants belong under
``lib/firmware/rtl_nic/``. The bounded userspace test brings up the discovered
wireless interface and performs a passive scan with ``iw``; it does not join
a network. Results are in ``/tmp/usb-wifi-scan.txt`` with a summary in printk.

Cleanroom source contracts used for this experimental path:
``NEO_USB2_FOR_CLEANROOM.md``, ``NEO_USB2_IMPLEMENTATION_SUPPLEMENT.md``,
``NEO_USB2_PIPE_DUMMY_LOCK_SUPPLEMENT.md`` and
``NEO_USB2_FRONT_HPM1_FOR_CLEANROOM.md``. These describe behavior audited at
``bedd2558acdf8deef4bd4ad2c3cf41873f43a7df``; they were not source transplants.
The PIPE bootstrap must enable its dummy backend, not just select the mux.
Repeated dummy selection is verified and idempotent; an unnecessary live
lock handshake was the first observed host-startup failure.

Host-side checks in ``linux-enablement-mac-alpha/tests/`` exercise actual
repeater/PIPE/role C functions with fault injection and compile all three DT
profiles. They are software tests, not hardware emulation or qualification.

Required sibling repositories are m1n1-alpha, linux-alpha and grub-alpha.
The optional touchpad fixture also needs linux-enablement-mac-alpha for its
evdev monitor and framebuffer demo sources.
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

Optional touchpad test
---------------------

Add ``--touchpad-firmware /absolute/path/to/tpmtfw-j700.bin`` to the build
command. Without this argument the keyboard-only fixture remains unchanged.
With it, the builder includes the firmware at
``/lib/firmware/apple/tpmtfw-j700.bin``, enables the existing SMC/GPIO and
``hid-magicmouse`` drivers, and cross-compiles the sibling
``linux-enablement-mac-alpha/tools/touchpad-demo.c`` and ``touchpad-events.c``
as static executables.
Linux must include the J700/C1FE report decoder in ``hid-magicmouse``.
The ``multi-touch`` child compatible ``apple,j700-multitouch`` selects it;
other machines retain the legacy report layout. No GRUB input change is needed.

The init script selects virtual terminal 1 and automatically starts
``/bin/touchpad-demo`` there with an explicit controlling terminal. It uses
ordinary fbdev/evdev interfaces, discovers the touchpad by name, queries its
axis ranges and draws numbered colored contact dots. The border turns green
while the primary button is down. The displayed count should return to zero
when all fingers lift. Positions are absolute contacts, not an accelerated
desktop cursor. No scrollback spam or compositor is involved.

The viewer accepts the framebuffer's actual 32-bit RGB bitfields and stride,
including 10-bit channels, validates its mapped extent, and limits drawing to
30 frames per second. The optional fixture explicitly enables and checks
``CONFIG_FB_DEVICE``: framebuffer console support alone does not expose
``/dev/fb0`` for userspace drawing. It updates only complete input frames and
queries slot state after ``SYN_DROPPED``. Q, Esc or Ctrl-C returns to the shell and restores
text mode and terminal settings. Re-run ``/bin/touchpad-demo`` for graphics or
``/bin/touchpad-events`` for the original raw event monitor. Concise count,
button and frame summaries remain available through the kernel/UART log.

For host-only demo checks, compile ``touchpad-demo.c`` with
``-Wall -Wextra -Werror -fsanitize=address,undefined`` and run ``--self-test``.
This tests pixel packing, mapping bounds, rendering and evdev state handling;
it is not a physical framebuffer or trackpad test. Raw evdev contacts do not
qualify desktop gestures, libinput or palm rejection.

Firmware provenance for the first test was the local
``UniversalMac_26.6.2_25G83_Restore.ipsw``. Its J700 BuildManifest identity names
``Firmware/J700_Multitouch.im4p``. The extracted ``mtfw`` plist contains the
single ``C1FE0,0`` personality, matching the captured J700 ADT. The existing
Asahi installer HIDF serializer can encode that selected entry directly; its
older automatic C1FD-only filter would miss this machine. Do not substitute
another Mac's firmware or send the raw IM4P/XML to the driver. This is
machine-selected firmware, not proof of additional per-unit personalization.
No executable firmware was disassembled.

The derived HIDF is 75544 bytes, SHA256
``9d17f2a0f9fb17f45ac4d2da94fb0e0296d7c1a9511fc799411b6b058c7d7c74``.
The blob remains installation data and is not committed. Envelope validation
rejects truncated headers, unsupported formats and out-of-bounds lengths or
interface patch offsets. Run ``python3 tools/apple_j700_first_light_test.py``
for the host-only packaging checks.

The optional DT describes SMC core 0x30c600000, mailbox 0x30c608000 and the
captured nub SRAM region 0x30de00000/+0x100000. Mailbox IRQs 581..584 are in
semantic empty/not-empty order. SMC follows its ordinary RTKit lifecycle;
it is independent of the retained MTP session. AFE uses the documented J700
SMC GPIO 0x13 active-low selection, not J713's GPIO. Existing PowerMethod 2
handles the firmware transfer and AFE transition. No speculative ``pmIP``
writes are introduced; this fixture relies on earlier boot stages for the
existing IPD power state and does not qualify a cold IPD power-up or suspend.

The initial hardware run registered both input interfaces, initialized SMC
GPIO, transferred firmware and reported readiness at 5.75 seconds, but no evdev
motion appeared. Continuous capture on 2026-09-07 subsequently proved that
runtime 0x75 reports reached Linux: the old 38-byte-header decoder rejected
Neo's 32-byte header, 30-byte contact records and 8-byte opaque suffix.
All 72 complete runtime samples independently validated this framing using
the section lengths and record count in the header. No new enable command
or firmware change was needed. Temporary raw report logging is removed.

The superseding cleanroom parser handoff follows the header count for contact
presence, not opaque candidate area/state fields. It reuses the existing signed
coordinate and primary-button convention and synchronizes zero-contact releases.
It advertises no unverified touch size, tool width, orientation or pressure axes.
Legacy machines keep their area-based filtering and axes. Strict framing is the
captured C1FE profile, not a rule proved for every future firmware. The random
capture establishes framing, not physical axis calibration or click meaning.
The self-contained contract is in the sibling kernel repository at
``Documentation/hid/apple-j700-multitouch.rst``; temporary handoff documents
are not needed to interpret or reproduce the supported parser profile.
Host ASan/UBSan replay and legacy tests are in
``linux-enablement-mac-alpha/tests/test_magicmouse_c1fe.py``.

On 2026-09-07 the revised kernel and framebuffer demo were RAM-booted through
stage2, U-Boot and GRUB. The demo opened the 2408x1506 framebuffer and 16 evdev
slots with queried X bounds -5499..5499 and Y bounds -6469..164. The operator
confirmed that it worked perfectly, after previously confirming the keyboard.
This is functional first-light acceptance, not formal axis calibration or
qualification of desktop gestures, palm rejection, suspend or cold takeover.

The validated EFI image was 11112448 bytes, SHA256
``d9ba31001377493e44e2eb9f1670588ebad2e8912dd25dc9e173db2eab534e5c``;
the RAM chain was 6185197 bytes, SHA256
``9937d6ec4027668493185463af3e63a93f95405617c2ce401d6bb3efd6598b2a``.
These identify the hardware-tested build, not a promise that timestamped
rebuilds will be byte-identical. Host validation passed 6065 parser checks,
four firmware-envelope tests and the framebuffer demo self-test. Kernel
checkpatch reported no errors or warnings. No installed boot files were changed.

SMC emitted unknown OSLog messages, and omitted optional MFD children emitted
missing-node messages; neither blocked GPIO initialization. This is not
qualification of those unimplemented SMC consumers.
