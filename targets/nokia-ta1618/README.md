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
operation are validated on physical TA-1618 hardware. The current portrait
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
switched to a 4-bit bus at 13 MHz, and read and written in transfers of up to
256 blocks, with the controller issuing the stop command itself. A FAT32
partition mounts read-write, and files survive an unmount and a remount byte
for byte.

What is deliberately absent:

- **Hot-swap.** The board does have a card-detect pin, but this driver never
  reads it. The card must be inserted before Linux starts, and removing it
  while mounted is not supported.
- **Bus speed.** The card runs at 13 MHz. A faster bus is proven on this board
  but is not part of this driver yet, so the card still reaches roughly half of
  what the wire could carry.
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
| Display                           | Partial       | 240×320 RGB565 portrait; exact current closure awaits phone gate   |
| Physical keypad                   | Supported     | Polled matrix plus separate physical 8 key through analog EIC9/ADI |
| USB device mode                   | Supported     | MUSB peripheral mode with `g_serial` at USB ID `0525:a4a6`         |
| USB host mode                     | Not supported | The phone target enables peripheral mode only                      |
| microSD card                      | Supported     | 4-bit 13 MHz multi-block reads and writes; no hot-swap             |
| Internal flash access             | Not supported | Linux does not expose phone storage                                |
| Audio                             | Not supported | No speaker, headphone or microphone driver is implemented          |
| Modem, calls, SMS and mobile data | Not supported | Baseband interfaces are not implemented                            |
| Bluetooth and Wi-Fi               | Not supported | Connectivity drivers are not included                              |
| Battery and charging              | Not supported | Power-supply hardware is not described                             |
| Suspend and power management      | Not supported | Kernel suspend support is disabled                                 |
| Camera                            | Not supported | No camera pipeline or sensor driver is included                    |
| LEDs and vibration motor          | Not supported | Not described by the current target                                |

## Drivers

All drivers required by the console profile are built into the kernel image:

```text
CONFIG_ARCH_UMS9117=y
CONFIG_MACH_NOKIA_TA1618=y
CONFIG_MFD_SYSCON=y
CONFIG_KEYBOARD_TA1618=y
CONFIG_FB_TA1618=y
CONFIG_USB_MUSB_UMS9117=y
CONFIG_USB_G_SERIAL=y
```

The runtime does not depend on `/lib/modules`, `modprobe` or a persistent root
filesystem. The display and keypad drivers expose standard `fbcon` and evdev
interfaces to the [shared FPLinux terminal](../../docs/porting/CONSOLE.md); the
terminal contains no TA-1618 register or scan-code logic. The target bootstrap
uses the shared freestanding
[boot-screen renderer](../../bootstrap/fplinux-boot-screen/) while retaining its
panel adapter, hardware stages and handoff diagnostics locally.

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

`Ctrl-]` detaches the host console without stopping the phone shell. If Linux is
still running, reconnect directly with:

```sh
.cache/out/nokia-ta1618/console/host/fplinux-usb-console
```

Do not start the full runner merely to reconnect. To end the RAM session, detach
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

- The bootstrap configures the ST7789P3 panel for `240×320` portrait output and
  prepares the display state inherited by Linux. The framebuffer driver does not
  perform complete cold initialization of every display clock, reset and
  regulator.
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
