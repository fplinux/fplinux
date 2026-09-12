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
ROTA, JPEG codec/scaler, analog-die, framebuffer and matrix-keypad support for
the listed phones. Linux also reads and
reports the inherited MPLL and Cortex-A7 rates without changing them. The
platform owns the SoC integration and shared loader support. A target
owns board memory, panel profile and wiring, keypad map, bootstrap inputs,
payload assembly and the values supplied to the loader.

## Reusable capabilities

| Capability                                                     | Status        | Target-facing requirement or limitation                                                                     |
| -------------------------------------------------------------- | ------------- | ----------------------------------------------------------------------------------------------------------- |
| CPU / GIC / timers                                             | Supported     | The SoC has one Cortex-A7 CPU; targets use the shared interrupt and timer nodes.                            |
| [Clock controller](../../docs/features/CPU_CLOCK.md)           | Partial       | Linux reads and reports inherited MPLL/Cortex-A7 rates; it does not change clocks.                          |
| [USB device controller](../../docs/features/USB_NETWORKING.md) | Supported     | USB peripheral support is shared; board USB setup remains target-owned.                                     |
| USB host mode                                                  | Not supported | The shared MUSB integration is peripheral-only and provides no host-mode initialization.                    |
| [AP DMAengine](#memory-copy-dma)                               | Partial       | 32 shared channels for memory copies and the fixed ROTA request; client availability depends on the target. |
| [Image rotation](../../docs/apps/ROTATE.md)                    | Supported     | V4L2 mem2mem ROTA with a shared userspace interface.                                                        |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)              | Supported     | Baseline JPEG decode, fixed quality-85 JPEG encode, and two fixed half-size NV16 scaler pairs.              |
| [Native image presentation](../../docs/apps/PRESENT.md)        | Supported     | Native-size NV16 or RGB565 presentation through the target framebuffer.                                     |
| Analog-die interface                                           | Supported     | Linux initializes the shared transport; feature clients and their board wiring remain target-owned.         |
| [LCDC framebuffer core](../../docs/features/LOCAL_CONSOLE.md)  | Supported     | Targets provide a panel profile and the board-specific panel transport setup.                               |
| [Matrix keypad](../../docs/features/LOCAL_CONSOLE.md)          | Supported     | Targets provide matrix wiring, inherited EIC use where applicable, and the normalized keymap.               |
| Audio playback                                                 | Not supported | No audio driver is included.                                                                                |
| UART, GPIO/pin control, SPI/I2C, watchdog/reset                | Not supported | No generic platform framework or driver for these functions.                                                |

## Memory-copy DMA

The AP DMA controller provides 32 channels through the standard Linux DMAengine
`DMA_MEMCPY` interface. It supports arbitrary byte alignment, queued transfers,
interrupt-driven completion, pause/resume and synchronous termination. Transfers
larger than one hardware block are split transparently. Residue is reported at
completed-segment granularity, not as a live byte counter.

A configuration error aborts the active and queued transfers with `DMA_ERROR`
and `DMA_TRANS_ABORTED`. Release and request the channel again before reusing it.
If the controller cannot be stopped, termination returns an error; the client
must retain its DMA buffers until a cold boot. Error recovery is qualified for
the source-alignment fault; other hardware fault conditions are unqualified.

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

## Targets using this platform

| Target                                                             | Phone                   |
| ------------------------------------------------------------------ | ----------------------- |
| [`inoi-240-modern-4g`](../../targets/inoi-240-modern-4g/README.md) | INOI 240 Modern 4G      |
| [`inoi-244-modern-4g`](../../targets/inoi-244-modern-4g/README.md) | INOI 244 Modern 4G      |
| [`nokia-ta1618`](../../targets/nokia-ta1618/README.md)             | Nokia 3210 4G (TA-1618) |

Platform status covers shared capabilities; target documents own board-specific
status. Neither is release qualification.

See the [hardware platform index](../README.md), the
[porting overview](../../docs/porting/README.md), and the project
[documentation index](../../README.md#documentation) for the surrounding
contributor contracts and user workflows.
