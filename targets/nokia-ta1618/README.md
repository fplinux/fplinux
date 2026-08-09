# Nokia 3210 4G (TA-1618)

## Device

| Field             | Value                                                      |
| ----------------- | ---------------------------------------------------------- |
| Manufacturer      | HMD / Nokia                                                |
| Model             | Nokia 3210 4G                                              |
| Hardware variant  | TA-1618                                                    |
| Platform          | [Unisoc UMS9117 / T117](../../platforms/ums9117/README.md) |
| CPU used by Linux | ARM Cortex-A7, CPU0, 1 GHz                                 |
| FPLinux profile   | `console`                                                  |
| Boot method       | Volatile RAM through the Unisoc BootROM and FDL1           |
| Linux version     | 6.18.40                                                    |

## Current status

The current source builds a volatile-RAM Linux image with a `240×320` portrait
framebuffer, physical-keypad local shell and separate Linux USB shell. Boot,
initramfs, timers, interrupts, display, keypad and bidirectional USB console
operation are validated on physical TA-1618 hardware. Display updates are
damage-driven and complete through the LCDC interrupt: a settled framebuffer
causes no transfers, while active full-screen drawing reaches 46.7 frames per
second without visible tearing. Measured 4 MiB transfer rates with a static
display are 753 KiB/s for upload and 1.40 MiB/s for pull. The current portrait
terminal closure has not completed its exact phone-side release gate.

`releases.lock.toml` contains no qualified `nokia-ta1618` runtime closure, so no
prebuilt archive is currently available. A successful source build or a local
candidate package does not change that status.

## microSD card

The removable microSD slot is driven by a board-specific host on the UMS9117
SDIO0 instance. It is deliberately not an SDHCI driver: the register at offset
0x28 is a 32-bit custom host control word and there is no SDHCI power control
byte, so the driver programs the gate, reset, clock selector, pin and analog
rail recipe that was proven on this board.

What works: a card that is already inserted when Linux starts is identified,
switched to a 4-bit bus, and read and written in transfers of up to 256 blocks,
with the controller issuing the stop command itself. A card that agrees to SD
high speed runs at 48.75 MHz and one that does not runs at 24.375 MHz, both
below what the card specification permits for the mode. A FAT32 partition
mounts read-write, and files survive an unmount and a remount byte for byte.

Measured on two cards with `fio`, verifying a checksum on every block as it is
read back:

| Workload           | SanDisk SD16G | 32 GB card |
| ------------------ | ------------- | ---------- |
| Sequential read    | 19.6 MiB/s    | 5.98 MiB/s |
| Sequential write   | 8.73 MiB/s    | 8.19 MiB/s |
| Random 4 KiB read  | 434 IOPS      | 150 IOPS   |
| Random 4 KiB write | 267 IOPS      | 93 IOPS    |

Five minutes of mixed random traffic per card moved 3.6 GiB in total across
87000 commands without a single checksum, end-bit, timeout or descriptor error.
Swap on a file on the card works: pages are evicted under memory pressure and
read back unchanged.

What is deliberately absent:

- **Hot-swap.** The board does have a card-detect pin, but this driver never
  reads it. The card must be inserted before Linux starts, and removing it
  while mounted is not supported.
- **Faster signalling.** The controller reports that it can do the modes that
  need 1.8 V signalling, which would be several times faster again. Reaching
  them means switching the signalling rail through the analog companion, and
  this driver never writes to it.
- **Erase and discard.** Erase, discard, lock and write-protect commands are
  still refused before they can reach the controller.

The phone's internal storage is SPI NAND on a separate controller with its own
gate, reset and interrupt. It is not described in the device tree, no NAND or
MTD support is built, and the build refuses any kernel configuration or device
tree that would reach it. The host driver only ever writes the set and clear
aliases of the peripheral gate and reset banks, so it cannot disturb a
neighbouring controller even transiently.

The controller raises its own interrupt on the line the device tree names, and
that line has been counted live in `/proc/interrupts` on the phone, so the
numbering this board uses for the system timer and the USB controller holds
here too.

## Hardware support

**Supported** means the feature has been exercised on physical TA-1618 hardware
and its implementation is present in the current target. It does not mean the
current aggregate runtime closure is qualified as a release. **Partial** means
that the stated limitation or current qualification gap still applies.

| Area                              | Status        | Notes                                                              |
| --------------------------------- | ------------- | ------------------------------------------------------------------ |
| BootROM RAM loading               | Supported     | Fixed RAM-only sequence; no flash, erase, partition or NV command  |
| Linux kernel and initramfs        | Supported     | Linux 6.18.40 with a musl/BusyBox root filesystem                  |
| CPU                               | Supported     | The SoC has a single Cortex-A7 core, so SMP does not apply         |
| Interrupt controller              | Supported     | ARM GIC with working timer and USB interrupts                      |
| System timers                     | Supported     | UMS9117 system counter and Pike2 timer                             |
| Display                           | Supported     | Damage-driven RGB565; up to 46.7 frames/s without visible tearing  |
| Physical keypad                   | Supported     | Polled matrix plus separate physical 8 key through analog EIC9/ADI |
| USB device mode                   | Supported     | MUSB peripheral mode with `g_serial` at USB ID `0525:a4a6`         |
| USB host mode                     | Not supported | The phone target enables peripheral mode only                      |
| microSD card                      | Supported     | 4-bit multi-block reads and writes up to 48.75 MHz; no hot-swap    |
| Internal flash access             | Not supported | Linux does not expose phone storage                                |
| Audio                             | Not supported | No speaker, headphone or microphone driver is implemented          |
| Modem, calls, SMS and mobile data | Not supported | Baseband interfaces are not implemented                            |
| Bluetooth and Wi-Fi               | Not supported | Connectivity drivers are not included                              |
| Battery and charging              | Not supported | Power-supply hardware is not described                             |
| Suspend and power management      | Not supported | Kernel suspend support is disabled                                 |
| Camera                            | Not supported | No camera pipeline or sensor driver is included                    |
| Indicator LEDs and vibrator       | Not supported | WLED backlight works; no indicator LED or vibrator driver exists   |

## Drivers

All drivers required by the console profile are built into the kernel image:

```text
CONFIG_ARCH_UMS9117=y
CONFIG_MACH_NOKIA_TA1618=y
CONFIG_MFD_SYSCON=y
CONFIG_KEYBOARD_TA1618=y
CONFIG_FB=y
CONFIG_FB_TA1618=y
CONFIG_FRAMEBUFFER_CONSOLE=y
CONFIG_USB_MUSB_UMS9117=y
CONFIG_USB_G_SERIAL=y
```

The target-side input bridge uses these image selections:

```text
CONFIG_INPUT_EVDEV=y
CONFIG_INPUT_MISC=y
CONFIG_INPUT_UINPUT=y
BR2_PACKAGE_FPLINUX_INPUT=y
```

At boot, `/usr/libexec/fplinux/init` starts `/bin/fplinux-input`. The process
keeps one `FPLinux host keyboard` uinput device alive, reads event lines from
`/dev/ttyGS1` and releases pressed keys when the host disconnects or stops
sending events.

The runtime does not depend on `/lib/modules`, `modprobe` or a persistent root
filesystem. The display and keypad drivers expose standard `fbcon` and evdev
interfaces to the [shared FPLinux terminal](../../docs/porting/CONSOLE.md); the
terminal contains no TA-1618 register or scan-code logic. The target bootstrap
uses the shared freestanding
[boot-screen renderer](../../bootstrap/fplinux-boot-screen/) while retaining its
panel adapter, hardware stages and handoff diagnostics locally.

## Framebuffer update contract

The framebuffer exposes two contiguous `240×320` RGB565 pages. Each row is 480
bytes, each page is 153600 bytes, and the complete mapping is 307200 bytes. The
front page starts at byte 0 and the back page at byte 153600; select them with
`yoffset` 0 and 320 respectively. The page boundary is not aligned to the
kernel's 4096-byte pages, so map the complete region and address the back page as
`mapping + 153600` rather than trying to map it at that file offset.

Framebuffer writes and drawing operations used by fbcon report damage directly.
An application that writes through `mmap()` must publish each completed batch
itself: stop changing the submitted bytes, execute an architecture-appropriate
full memory barrier, then issue `FBIOPAN_DISPLAY`. The ioctl is required even
when `yoffset` remains unchanged. A direct mapped write without that notification
is intentionally silent and does not wake the display pipeline.

`FBIOPAN_DISPLAY` reports selection and damage; it is not a completion fence.
Updates may be coalesced, and the driver does not promise that every intermediate
image reaches the panel. Applications that require stable animation frames must
keep a submitted buffer unchanged while it can still be snapshotted and use two
fully populated pages. The second page starts with unspecified contents.

## Build from source

From the repository root:

```sh
./fplinux doctor
./fplinux build nokia-ta1618
```

The target is auto-discovered from its data-only `fplinux.target/v1` manifest.
The root command combines it with `platforms/ums9117/platform.toml` and invokes
the shared stages 1–4 builder. The runnable output is written to:

```text
.cache/out/nokia-ta1618/console/
```

See [Building FPLinux](../../docs/BUILDING.md) for host setup, cache recovery,
output layout and reproducibility details.

## Run from the source checkout

The generated Linux x86-64 host tools require Python 3.11 or newer, glibc 2.38
or newer, libusb 1.0, libudev and GNU `stdbuf` from coreutils. Install the
[documented udev rule](../../docs/RELEASES.md#usb-access) for USB IDs
`1782:4d00` and `0525:a4a6` before connecting the phone.

Power the phone off and disconnect USB, then start the runner:

```sh
./fplinux run nokia-ta1618
```

Before asking for the phone, the shared runner validates the generated
`runtime-manifest.json`, bundle hashes and host-tool dependencies. When prompted,
hold `*` and connect USB while keeping `*` pressed. The fixed UMS9117 adapter
verifies BootROM USB access, loads FDL1 and the FPLinux payload into RAM, then
attaches to the Linux USB shell. Addresses, USB identifiers, wait times,
board-asset roles and adapter values come from validated target data.

`Ctrl-]` detaches the host console without stopping the phone shell. From the
repository root, reconnect to interface 0 with:

```sh
./fplinux console nokia-ta1618
```

Forward a host keyboard on interface 1. The client grabs the selected evdev
node, so its keys do not reach the host desktop until the process exits:

```sh
sudo ./fplinux console nokia-ta1618 --keyboard /dev/input/eventN
```

Limit a forwarding session to 60 seconds with GNU `timeout`:

```sh
sudo timeout 60s ./fplinux console nokia-ta1618 --keyboard /dev/input/eventN
```

After 60 seconds, `timeout` sends `SIGTERM`; the client releases the evdev grab
before it exits.

The keyboard forwarder and one interface 0 client can run at the same time. Do
not start the full runner merely to reconnect. To end the RAM session, detach
with `Ctrl-]`, disconnect USB, remove the battery and then reinsert it. The next
normal power-on uses the unchanged vendor firmware. Linux `reboot`, `poweroff`
and PMIC-controlled shutdown are not qualified exit paths. See
[Console lifecycle](../../docs/RELEASES.md#console-lifecycle) for key behavior and
troubleshooting.

## Release archive

No prebuilt archive is currently available. Maintainers can create a local
hardware-qualification candidate after a successful build:

```sh
./fplinux package nokia-ta1618 --candidate
```

The Linux x86-64 candidate appears under `.cache/out/candidates/`. The package
command without `--candidate` is reserved for a runtime closure whose exact
SHA-256 has completed the phone gate and is recorded in `releases.lock.toml`.
The generic [release contract](../../docs/RELEASES.md) defines package contents,
host requirements and release rules.

A Linux x86-64 release archive is run from its extracted top-level directory:

```sh
cd FPLinux-Nokia-3210-4G-TA1618-console-release-linux-x86_64-<content16>
./runner/run.py
```

The current host loader requires glibc 2.38 or newer, Python 3.11 or newer,
libusb 1.0, libudev, GNU `stdbuf` and USB permissions for `1782:4d00` and
`0525:a4a6`.

## Phone-specific nuances

- The Linux framebuffer driver configures the display pins, SPI1 and LCDC clocks
  and resets, resets the ST7789P3, sends the complete panel initialization
  sequence and brings up WLED. Linux initializes the display independently of
  the bootstrap. Blank and unblank quiesce the LCDC pipeline, use DCS Display
  Off/On and Sleep In/Out, and restore WLED only after a full wake frame reaches
  `LCDC_DONE`.
- Each frame starts on the panel's tearing signal and takes 10.5 ms of link time
  at 88 MHz. The panel is held at 46.7 Hz so that one frame lands inside one pass
  of its scan, which is what removes tearing rather than merely hiding it.
- Linux uses RAM at `0x80000000..0x83dfffff`. The DTB, handoff diagnostics and
  framebuffer occupy reserved regions above it.
- The payload is loaded at `0x80100000`; `0x82000000` is the zImage staging
  boundary.
- MUSB runs in PIO peripheral mode. USB DMA and host mode are not implemented.
- The physical `8` key is not part of the keypad matrix. Linux polls the
  inherited analog EIC9 level through the ADI hardware lock and reports it as
  `KEY_8`; the driver does not reconfigure EIC polarity or enable state.
- Board maps and FDL1 are downloaded and SHA-256 checked during source builds;
  see [`loader/PROVENANCE.md`](loader/PROVENANCE.md).
- Runtime state is volatile. The tested TA-1618 exit is to detach the host
  client, disconnect USB, remove the battery and then reinsert it. The next
  normal power-on uses the unchanged vendor firmware. Linux reboot, power-off,
  PMIC shutdown and power-button behavior are not qualified.

## Source layout

| Path          | Responsibility                                                 |
| ------------- | -------------------------------------------------------------- |
| `target.toml` | Data-only board inputs, identity and runtime adapter values    |
| `dts/`        | Phone memory map and enabled board devices                     |
| `kernel/`     | Display, keypad and hardware handoff support                   |
| `bootstrap/`  | Target RAM payload source and Linux handoff                    |
| `rootfs/`     | Target identity and boot diagnostics layered over common init  |
| `loader/`     | Generic `fplinux.assets/v1` lock and asset provenance          |
| `release/`    | Data-only `fplinux.release/v1` allowlist and archive documents |

Build behavior is shared in `scripts/fplinux_cli/builder.py`; execution uses
`common/run.py` and the fixed `platforms/ums9117/host/adapter.py`.
