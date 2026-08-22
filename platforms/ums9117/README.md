# Unisoc UMS9117 / T117 platform

## Identity

| Field                 | Value                            |
| --------------------- | -------------------------------- |
| Vendor                | Unisoc / Spreadtrum              |
| SoC                   | UMS9117, also identified as T117 |
| Architecture          | ARMv7-A                          |
| CPU                   | ARM Cortex-A7                    |
| Linux platform symbol | `CONFIG_ARCH_UMS9117`            |
| DTS compatible        | `sprd,ums9117`                   |

## Scope

UMS9117 provides reusable CPU, interrupt, timer, USB gadget, analog-die,
framebuffer and matrix-keypad support for the listed phones. The platform owns
the SoC integration and its fixed RAM-only loader flow. A target owns board
memory, panel profile and wiring, keypad map, bootstrap inputs, payload assembly
and the values supplied to the loader.

## Reusable capabilities

| Capability                                             | Status        | Target-facing requirement or limitation                                                                                                                                       |
| ------------------------------------------------------ | ------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| CPU / GIC / timers                                     | Supported     | The SoC has one Cortex-A7 CPU; targets use the shared interrupt and timer nodes.                                                                                              |
| Clock controller                                       | Partial       | Fixed clock nodes are available; there is no general clock-controller driver.                                                                                                 |
| USB device controller                                  | Supported     | PIO peripheral gadget mode only. The current Nokia cold-owned profile and both INOI inherited-state profiles have enumerated at USB High-Speed and exposed the serial gadget. |
| Analog-die interface                                   | Supported     | Linux initializes the shared transport; feature clients and their board wiring remain target-owned.                                                                           |
| LCDC framebuffer core                                  | Supported     | Targets provide a panel profile and the board-specific panel transport setup.                                                                                                 |
| Matrix keypad                                          | Supported     | Targets provide matrix wiring, inherited EIC use where applicable, and the normalized keymap.                                                                                 |
| USB host / DMA                                         | Not supported | No host-mode initialization or USB DMA path.                                                                                                                                  |
| UART, GPIO/pin control, audio, SPI/I2C, watchdog/reset | Not supported | No generic platform framework or driver for these functions.                                                                                                                  |

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

## Target requirements

Targets enable only the SoC nodes their board can use. MUSB is disabled by
default. INOI targets enable its inherited-state peripheral profile. Nokia
overrides the shared node with its cold-owned peripheral profile and the
SC2720 USB 3.3 V supply. Targets add board devices and do not repeat the SoC
timer, GIC or USB addresses.

## Known constraints

- The platform brings up one CPU only.
- USB uses PIO gadget mode. INOI targets use inherited controller state; Nokia
  initializes the controller, PHY and 3.3 V supply from Linux. On the current
  artifacts, all three target profiles enumerated at High-Speed and completed
  a shell/data-pull plus physical USB reconnect. This does not establish USB
  host, DMA, host-keyboard or application-workflow support.
- Clock, reset, pin-control and power-domain frameworks are not implemented.
- The platform has no USB host or DMA support.

## Targets using this platform

| Target                                                             | Phone                   | Enabled shared capabilities                                       |
| ------------------------------------------------------------------ | ----------------------- | ----------------------------------------------------------------- |
| [`inoi-240-modern-4g`](../../targets/inoi-240-modern-4g/README.md) | INOI 240 Modern 4G      | CPU, GIC, timers, USB gadget, ADI, LCM/DBI framebuffer and keypad |
| [`inoi-244-modern-4g`](../../targets/inoi-244-modern-4g/README.md) | INOI 244 Modern 4G      | CPU, GIC, timers, USB gadget, ADI, LCM/DBI framebuffer and keypad |
| [`nokia-ta1618`](../../targets/nokia-ta1618/README.md)             | Nokia 3210 4G (TA-1618) | CPU, GIC, timers, USB gadget, ADI, SPI framebuffer and keypad     |

Platform capability status is not release qualification. Each target document
records its own physical-hardware evidence and runtime qualification state.
