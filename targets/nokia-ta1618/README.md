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
| Linux version     | 6.18.42                                                    |

## Current status

The current source builds a volatile-RAM Linux image with a `240×320` portrait
framebuffer, physical-keypad shell and two Linux USB serial interfaces. Interface
0 carries the USB shell and file-transfer modes. Interface 1 carries host
keyboard events into a persistent uinput device. The local terminal accepts up
to four compatible evdev devices, starts in T9 multi-tap mode, supports translated
QWERTY input and reserves the bottom row for mode and modifier status.

Boot, initramfs, timers, interrupts, display, keypad, keypad backlight, both USB
interfaces and the host keyboard bridge are validated on physical TA-1618
hardware. Display updates are damage-driven and complete through the LCDC
interrupt: a settled framebuffer
causes no transfers, while active full-screen drawing reaches 46.7 frames per
second without visible tearing. Measured 4 MiB transfer rates with a static display
are 753 KiB/s for upload and 1.40 MiB/s for pull. The current portrait terminal
closure has not completed its exact phone-side release gate.

`releases.lock.toml` contains no qualified `nokia-ta1618` runtime closure, so no
prebuilt archive is currently available. A successful source build or a local
candidate package does not change that status.

## TyrQuake

The target root filesystem includes the TyrQuake 0.71 engine without Quake game
data. Put a legally obtained PAK on a microSD card inserted before boot:

```text
/mnt/card/fplinux/quake/id1/pak0.pak
```

The launcher mounts the card read-only when `/mnt/card` is not already mounted.
Configuration and save files stay below `/tmp`; the game never writes through
the PAK links to the card. Start one of the two fixed input modes from the phone
shell:

```sh
quake --input phone
quake --input keyboard
```

Both TyrQuake input modes work on physical TA-1618 hardware. The `--input` flag
is the only source selector. Phone mode requires exactly one
physical keypad exposing the stable `fplinux/keypad0` evdev physical identity;
keyboard mode requires exactly one FPLinux host-keyboard uinput device created by
USB interface 1. Capabilities validate the selected class but never choose it.
The choice stays fixed until the game exits. TyrQuake grabs both recognized
sources when present, sends only the selected source to the game and discards the
other one. A missing or duplicate selected device is an error; there is no
fallback or hot switching between modes.

Phone controls assume that the phone is held counter-clockwise with the screen on
the left and keypad on the right. The directional layout is rotated into the
landscape game view:

| Key                 | Menu                 | Game                        |
| ------------------- | -------------------- | --------------------------- |
| D-pad `UP` / `DOWN` | Left / right         | Turn left / right           |
| D-pad `LEFT`        | Down                 | Walk backward               |
| D-pad `RIGHT`       | Up                   | Walk forward                |
| Centre or dial      | Select               | Fire                        |
| Right soft          | Back                 | Menu                        |
| Left soft or `*`    | -                    | Jump                        |
| `0`                 | -                    | Fire                        |
| `1` / `3`           | -                    | Strafe left / right         |
| `2` / `5`           | -                    | Turn left / right           |
| `4` / `6`           | -                    | Walk backward / forward     |
| `7` / `9`           | -                    | Previous / next weapon      |
| `8`                 | -                    | Run while held              |
| `#`                 | Available for a bind | Available for a custom bind |

Centre and dial both arrive from the kernel as `KEY_ENTER`, so the game cannot
distinguish them. Keyboard mode uses normal Quake keyboard controls and does not
apply T9 or terminal QWERTY translation.

The engine runs with a fixed 32 MiB heap, null sound and CD backends, and LAN
disabled. A supervising launcher owns the game session, forwards termination
signals and restores both framebuffer pages, the original framebuffer geometry,
the active VT and text mode after the engine exits. `SIGKILL` or power loss can
still bypass process cleanup.

## microSD card

The removable microSD slot is driven by a board-specific host on the UMS9117
SDIO0 instance. It is deliberately not an SDHCI driver: the register at offset
0x28 is a 32-bit custom host control word and there is no SDHCI power control
byte, so the driver programs the gate, reset, clock selector, pin and analog
rail recipe that was proven on this board.

### Mounting a card

Insert the card before Linux starts. Use the first partition when the card has a
partition table, or the whole-card device otherwise:

```sh
card=/dev/mmcblk0p1
[ -b "$card" ] || card=/dev/mmcblk0
mkdir -p /mnt/card
```

Mount it read-write for ordinary file access:

```sh
mount -t vfat -o rw "$card" /mnt/card
```

Use a read-only mount for game data and application archives:

```sh
mount -t vfat -o ro,nodev,nosuid,noexec,utf8=1 "$card" /mnt/card
```

The Quake launcher uses the read-only form when `/mnt/card` is not mounted. It
uses an existing mount with its current flags. Before ending the RAM session or
removing the card, flush pending writes and unmount it:

```sh
sync
umount /mnt/card
```

Do not remove the card while it is mounted; hot-swap is not supported.

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
| Linux kernel and initramfs        | Supported     | Linux 6.18.42 with a musl/BusyBox root filesystem                  |
| CPU                               | Supported     | The SoC has a single Cortex-A7 core, so SMP does not apply         |
| Interrupt controller              | Supported     | ARM GIC with working timer and USB interrupts                      |
| System timers                     | Supported     | UMS9117 system counter and Pike2 timer                             |
| Display                           | Supported     | Damage-driven RGB565; up to 46.7 frames/s without visible tearing  |
| Physical keypad                   | Supported     | Matrix plus active-low SC2720 EIC1 power and EIC9 physical 8       |
| Keypad backlight                  | Supported     | Any new key press starts or extends the five-second illumination   |
| USB device mode                   | Supported     | Two `g_serial` ports at `0525:a4a6`: shell/transfer and input      |
| Host keyboard bridge              | Supported     | Host evdev to `/dev/ttyGS1` to uinput; `EVIOCGRAB` on the host     |
| USB host mode                     | Not supported | The phone target enables peripheral mode only                      |
| microSD card                      | Supported     | 4-bit multi-block reads and writes up to 48.75 MHz; no hot-swap    |
| System power-off                  | Supported     | Hold red handset 5 s on battery; charger preflight blocks shutdown |
| Internal flash access             | Not supported | Linux does not expose phone storage                                |
| Audio                             | Not supported | No speaker, headphone or microphone driver is implemented          |
| Modem, calls, SMS and mobile data | Not supported | Baseband interfaces are not implemented                            |
| Bluetooth and Wi-Fi               | Not supported | Connectivity drivers are not included                              |
| Battery and charging              | Not supported | Power-supply hardware is not described                             |
| Suspend and power management      | Not supported | Kernel suspend support is disabled                                 |
| Camera                            | Not supported | No camera pipeline or sensor driver is included                    |
| Indicator LEDs and vibrator       | Not supported | No indicator LED or vibrator driver is implemented                 |

## Drivers

All drivers required by the console profile are built into the kernel image:

```text
CONFIG_ARCH_UMS9117=y
CONFIG_MACH_NOKIA_TA1618=y
CONFIG_MODULES=y
CONFIG_MODULE_UNLOAD=y
CONFIG_INPUT_EVDEV=y
CONFIG_INPUT_MISC=y
CONFIG_INPUT_UINPUT=y
CONFIG_KEYBOARD_UMS9117=y
CONFIG_FB=y
CONFIG_FB_TA1618=y
CONFIG_FRAMEBUFFER_CONSOLE=y
CONFIG_USB_MUSB_HDRC=y
CONFIG_USB_MUSB_UMS9117=y
CONFIG_MUSB_PIO_ONLY=y
CONFIG_USB_GADGET=y
CONFIG_USB_G_SERIAL=y
CONFIG_MMC=y
CONFIG_MMC_TA1618=y
CONFIG_NEW_LEDS=y
CONFIG_LEDS_CLASS=y
CONFIG_LEDS_TA1618_KPLED=y
CONFIG_FILE_LOCKING=y

BR2_PACKAGE_FPLINUX_CONSOLE=y
BR2_PACKAGE_FPLINUX_INPUT=y
BR2_PACKAGE_FPLINUX_TYRQUAKE=y
```

The kernel command line selects two non-ACM generic-serial ports with
`g_serial.use_acm=0 g_serial.n_ports=2`. At boot, the common
`/usr/libexec/fplinux/init` starts `/bin/fplinux-input`. The bridge creates the
`FPLinux host keyboard` uinput device, reads event lines from `/dev/ttyGS1` and
keeps the device alive across host disconnections.

The display, keypad, keypad-backlight, MMC and power-off drivers are built into the
kernel. Module loading and unloading are enabled for RAM-only driver work, but the
runtime has no `/lib/modules`, module dependency database or persistent root
filesystem. A test module must be transferred into `/tmp`, loaded explicitly with
`insmod` and removed with `rmmod` before the RAM session ends. The display, keypad
and keypad backlight expose standard `fbcon`, evdev and LED class interfaces to the
[shared FPLinux terminal](../../docs/porting/CONSOLE.md), while MMC exposes its
block device. The terminal contains no TA-1618 register or scan-code logic.
The target bootstrap supplies a board descriptor and payload assembly to the
shared UMS9117 boot flow. The platform flow owns the boot-screen renderer,
framebuffer adapter, timer probe, image staging, USB quiesce and Linux handoff.

## Keypad backlight

The physical keypad backlight is a binary Linux LED class device:

```sh
echo 1 > /sys/class/leds/:kbd_backlight/brightness
echo 0 > /sys/class/leds/:kbd_backlight/brightness
```

`max_brightness` is `1`. Each new press from the exact `TA-1618 keypad` input
device turns the backlight on and restarts its cutoff; key releases and autorepeat
events do not extend it. Writing `1` manually selects the board-qualified SC2720
current code `1` and starts the same in-kernel cutoff, which returns the output to
`0` after about five seconds. Writing `0` turns it off immediately. The driver
restores only its owned fields in `KPLED_CTRL0`; it does not write `KPLED_CTRL1`.

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
the shared stages 1–4 builder. The current runnable bundle is selected through:

```text
.cache/out/nokia-ta1618/console.current.json
```

The selected bundle is below
`.cache/out/nokia-ta1618/bundles/console/<generation>/`. When the recorded build
inputs match, `build` reports a cached result; otherwise it runs the normal build
stages. Build logs and their `run.json` record are under
`.cache/logs/build/nokia-ta1618/<run-id>/`.

See [Building FPLinux](../../docs/BUILDING.md) for host setup, cache inventory,
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

`Ctrl-]` detaches the host console without stopping the phone shell. Before
measuring, compare the phone's build stamp with the local bundle receipt:

```sh
./fplinux verify nokia-ta1618
```

`verify` first refuses a stale local bundle, then compares the Buildroot recipe
in `/etc/fplinux-build` and the kernel suffix with the local
`build-manifest.json`. The suffix covers the prepared Linux, rootfs, kernel
configuration, DTB and bootstrap recipe. It does not verify the other bundle
files or qualify the hardware.

From the repository root, reconnect to the shell:

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
not start the full runner merely to reconnect.

To end the RAM session through the qualified SC2720 path, detach with `Ctrl-]`,
disconnect USB and hold the red handset key continuously for five seconds.
Releasing it before the deadline cancels the request; a short press remains an
ordinary `KEY_POWER` event.

The target handler permits shutdown only after the exact PMIC identity and
charger-status preflight pass, then syncs the filesystems. The final sys-off
handler repeats both guards before its single final PMIC write. If shutdown has
started but the phone remains powered, remove and reinsert the battery before booting
again. A successful power-off discards the volatile FPLinux session, and the
next manual power-on uses the unchanged vendor firmware. Linux reboot remains
unsupported. See [Console lifecycle](../../docs/RELEASES.md#console-lifecycle)
for the complete failure boundary.

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
- Linux uses RAM at `0x80000000..0x83dfffff`. DTB staging starts at
  `0x83e00000`, and the framebuffer occupies `0x83f00000..0x83ffffff`.
- The payload is loaded at `0x80100000`; `0x82000000` is the zImage staging
  boundary.
- MUSB runs in PIO peripheral mode. EP1 backs USB interface 0 for shell and
  transfers; EP2 backs interface 1 for input events. USB DMA and host mode are
  not implemented.
- The red handset key and physical `8` key are outside the keypad matrix. Linux
  samples their inherited SC2720 analog EIC1 and EIC9 levels through the shared
  UMS9117 ADI provider and reports them as `KEY_POWER` and `KEY_8`. The driver
  does not reconfigure EIC polarity or enable state.
- The shared ADI provider owns the fixed controller and analog-slave mappings.
  It validates the inherited controller state and serializes framebuffer WLED,
  keypad EIC, keypad-backlight, MMC rail and SC2720 power-off transactions under
  the hardware user lock.
- Board maps and FDL1 are downloaded and SHA-256 checked during source builds;
  see [`loader/PROVENANCE.md`](loader/PROVENANCE.md).
- Runtime state is volatile. Holding the red handset key for five seconds
  requests the qualified battery-only power-off path; releasing it sooner
  cancels the request. The next manual power-on uses the unchanged vendor
  firmware. Linux reboot is not qualified.

## Source layout

| Path          | Responsibility                                                |
| ------------- | ------------------------------------------------------------- |
| `target.toml` | Data-only board inputs, identity and runtime adapter values   |
| `dts/`        | Phone memory map and enabled board devices                    |
| `kernel/`     | Display, keypad, keypad-backlight, MMC and power-off support  |
| `bootstrap/`  | Board descriptor and target payload assembly                  |
| `rootfs/`     | Target rootfs configuration layered over the common overlay   |
| `loader/`     | Generic `fplinux.assets/v1` lock and asset provenance         |
| `release/`    | Data-only `fplinux.release/v2` runtime and archive allowlists |

Build behavior is shared in `scripts/fplinux_cli/builder.py`; execution uses
`common/run.py` and the fixed `platforms/ums9117/host/adapter.py`.
