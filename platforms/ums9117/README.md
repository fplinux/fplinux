# Unisoc UMS9117 platform

## Identity

| Field                  | Value                 |
| ---------------------- | --------------------- |
| Vendor                 | Unisoc                |
| SoC                    | UMS9117               |
| Vendor/reference alias | T117                  |
| Architecture           | ARMv7-A               |
| CPU                    | ARM Cortex-A7         |
| Linux platform symbol  | `CONFIG_ARCH_UMS9117` |
| DTS compatible         | `sprd,ums9117`        |

## Scope

UMS9117 provides reusable CPU, interrupt, timer, USB gadget, memory-copy DMA,
ROTA, JPEG codec/scaler, analog-die, audio, Bluetooth, FM radio, framebuffer,
matrix-keypad, microSD and read-only NAND support for the listed phones. Audio
covers headphone and speaker playback, microphone capture and FM playback. Linux
reports the MPLL and Cortex-A7 rates and offers manual selection between 768 MHz
and 1 GHz. The platform owns the SoC integration and shared loader support. A
target owns board memory, panel profile and wiring, keypad map, bootstrap
inputs, payload assembly and the values supplied to the loader.

## Reusable capabilities

| Capability                                                     | Status        | Target-facing requirement or limitation                                                                                                                  |
| -------------------------------------------------------------- | ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| CPU / GIC / timers                                             | Supported     | The SoC has one Cortex-A7 CPU; targets use the shared interrupt and timer nodes.                                                                         |
| [CPU clock](../../docs/features/CPU_CLOCK.md)                  | Partial       | Manual 768 MHz or 1 GHz selection retains the inherited voltage and MPLL; each target states whether switching has been tested.                          |
| [USB device controller](../../docs/features/USB_NETWORKING.md) | Supported     | USB peripheral support is shared; board USB setup remains target-owned.                                                                                  |
| USB host mode                                                  | Not supported | The shared MUSB integration is peripheral-only and provides no host-mode initialization.                                                                 |
| [AP DMAengine](#memory-copy-dma)                               | Partial       | 32 shared channels for memory copies and the fixed ROTA request; client availability depends on the target.                                              |
| [Image rotation](../../docs/apps/ROTATE.md)                    | Supported     | V4L2 mem2mem ROTA with a shared userspace interface.                                                                                                     |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)              | Supported     | Baseline JPEG decode, fixed quality-85 JPEG encode, and two fixed half-size NV16 scaler pairs.                                                           |
| [Native image presentation](../../docs/apps/PRESENT.md)        | Supported     | Native-size NV16 or RGB565 presentation through the target framebuffer.                                                                                  |
| Analog-die interface                                           | Supported     | Linux initializes the transport and shared SC2720 charger, fuel-gauge, RTC, ADC, keypad-light and power-off clients; the vibrator is enabled per target. |
| [LCDC framebuffer core](../../docs/features/LOCAL_CONSOLE.md)  | Supported     | Targets provide a panel profile and the board-specific panel transport setup.                                                                            |
| [Matrix keypad](../../docs/features/LOCAL_CONSOLE.md)          | Supported     | Targets provide matrix wiring and EIC keys; the [phone key codes](../../docs/reference/INPUT.md) are shared.                                             |
| [Audio](../../docs/features/HEADPHONE_AUDIO.md)                | Partial       | Headphones, microphones and [speaker audio](../../docs/features/SPEAKER_AUDIO.md) work on all targets; each target states its microphone limits.         |
| [Bluetooth](../../docs/features/BLUETOOTH.md)                  | Partial       | Shared CM4 HCI driver; each target needs firmware prepared from its own phone.                                                                           |
| [FM radio](../../docs/features/FM_RADIO.md)                    | Supported     | V4L2 receiver on the CM4 Bluetooth link, heard on the enabled outputs; it needs the phone's Bluetooth firmware and the target's fitted FM configuration. |
| [microSD host](../../docs/features/MICROSD.md)                 | Supported     | SD cards on a 4-bit bus with fixed pin settings; each target enables the slot.                                                                           |
| Internal NAND reader                                           | Partial       | Read-only raw page stream for [NAND backup](../../docs/guides/BUILDING.md#save-a-nand-backup); no filesystem, writes or erase.                           |
| SC2720 EIC GPIO                                                | Supported     | PMIC external-interrupt lines as GPIO; targets use them for keys.                                                                                        |
| Pin control and reset                                          | Partial       | Fixed microSD pin settings and the microSD controller reset line only.                                                                                   |
| UART, AP GPIO, I2C, general SPI, watchdog, system reset        | Not supported | No platform driver for these functions.                                                                                                                  |

## Memory-copy DMA

The AP DMA controller provides 32 channels through the standard Linux DMAengine
`DMA_MEMCPY` interface. It supports arbitrary byte alignment, queued transfers,
interrupt-driven completion, pause/resume and synchronous termination. Transfers
larger than one hardware block are split transparently. Residue is reported at
completed-segment granularity, not as a live byte counter.

A configuration error aborts the active and queued transfers with `DMA_ERROR`
and `DMA_TRANS_ABORTED`. Release and request the channel again before reusing it.
If the controller cannot be stopped, termination returns an error; the client
must retain its DMA buffers until a cold boot. Recovery from a source-alignment
fault has been tested; other hardware fault conditions have not.

Framebuffer DMA copies, when enabled by the target, and ROTA use one channel
each. They share the same DMA controller owner. This does not change the
framebuffer publication or page-flip contract.

ROTA uses its fixed peripheral request and hardware-generated external linked
lists through this controller. Generic peripheral requests, cyclic transfers,
caller-supplied hardware linked lists and a userspace memory-copy API are not
provided. Controller-private DMA
remains owned by its controller or target. See the target documentation for
the enabled clients and hardware support status of each phone.

## Shared framebuffer interface

The framebuffer exposes two pages. Framebuffer drawing paths publish their own
damage. An application writing through `mmap()` must stop changing a completed
batch, execute an architecture-appropriate full memory barrier, then issue
`FBIOPAN_DISPLAY` to publish that damage. It must do so even when selecting the
same page again.

`FBIOPAN_DISPLAY` selects a page and reports damage; it is not a completion
fence. Updates may be coalesced, so an application that needs stable animation
frames keeps a submitted page unchanged while the display pipeline can snapshot
it and uses fully populated alternate pages. A mapped write without publication
is intentionally silent.

All three phones expose the shared [LCD backlight](../../docs/features/DISPLAY_BACKLIGHT.md)
interface. The target selects the brightness range, panel transport and
initialization; consult its support status for physical brightness limits.

## Target requirements

Targets enable only the SoC nodes their board can use. Board devices and USB
setup remain target-owned.

A target without the loader display settings (`spi_mode`, `lcd_id`,
`backlight_channels` and `backlight_level` in `[adapter]`) and with a 0x0
bootstrap display loads headless: the panel and backlight stay untouched and
boot progress is reported only to the host. Without pin map and keymap loader
assets, the loader applies no board pin settings and leaves the keypad
uninitialized. Headless targets created by `./fplinux target new` reach the
USB session on the INOI 244 Modern 4G and the Nokia 3210 4G (TA-1618).

## Board data from the stock firmware

For a target that declares the `board-maps` device-data group, preparation
extracts a loader pin map, a keymap and a board report from the phone's own
stock firmware. It finds the stock application image through the partition
table in the NAND backup, using two agreeing VBM copies or else one PartI table,
and reads the image with the pinned `fphelper_t117` host tool. The pin map and
keymap are the tool's output unchanged. The board report contains:

- the keypad matrix with the [phone key codes](../../docs/reference/INPUT.md),
  the boot key, and the EIC9 candidate when exactly one standard phone key is
  absent from the matrix;
- every panel in the firmware's LCD list: its 16-bit ID, the controller name
  when the firmware names it, size, SPI or LCM interface, SPI clock or DBI
  timing, and the initialization commands and delays when the decoder can follow
  its init function;
- the pin settings of the display control, display data and audio pads;
- the white LED backlight level and the fuel-gauge current calibration;
- the NAND chip configurations listed by the firmware.

Every value names the firmware address or tool output it came from. A value
whose firmware structure is not found is listed as unresolved rather than
guessed. Preparation stops when the partition table, the signed application
image, the pin map or the keymap is missing, or when the VBM copies disagree.

The stock firmware does not record which panel is fitted, the keypad light
current, whether a vibration motor is fitted, or the CM4 firmware revision; a
person decides these. Keys wired to EIC lines other than the EIC9 candidate,
the candidate itself and the decoded panel commands must also be confirmed on
the phone.

## Targets using this platform

| Target                                                             | Phone                   |
| ------------------------------------------------------------------ | ----------------------- |
| [`inoi-240-modern-4g`](../../targets/inoi-240-modern-4g/README.md) | INOI 240 Modern 4G      |
| [`inoi-244-modern-4g`](../../targets/inoi-244-modern-4g/README.md) | INOI 244 Modern 4G      |
| [`nokia-ta1618`](../../targets/nokia-ta1618/README.md)             | Nokia 3210 4G (TA-1618) |

Platform status covers shared capabilities; target documents own board-specific
status. Neither makes an executable payload release-ready.

See the [hardware platform index](../README.md), the
[porting overview](../../docs/porting/README.md), and the project
[documentation index](../../README.md#documentation) for the surrounding
contributor contracts and user workflows.
