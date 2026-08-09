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

This platform supplies the reusable CPU, interrupt-controller, timer and USB
controller support needed by UMS9117 feature phones. It also declares the Linux
integration, bootstrap vendor projection, typed host-tool recipes and fixed
RAM-only host adapter used by the shared builder and runner. Phone memory maps,
panel configuration, keypad wiring, bootstrap source and adapter values stay in
individual targets.

## Platform contract

[`platform.toml`](platform.toml) is the declarative UMS9117 contract. Its Linux
section projects the platform Kconfig/Kbuild integration, drivers and DTSI. Its
bootstrap section selects the pinned fpdoom vendor subset and shared boot-screen
source. Typed host recipes build `spd_dump`, `libc_server` and the shared USB
console.

The runner contract bundles [`common/run.py`](../../common/run.py) with the fixed
[`host/adapter.py`](host/adapter.py). The shared runner verifies the generic
runtime manifest, hashes and host-tool shared-library dependencies. The adapter
accepts the `fplinux.host.ums9117-ram/v1` capability, checks `stdbuf` and BootROM
usbfs access, then performs only the fixed BootROM, FDL1, RAM payload, bootstrap
handoff and Linux USB-console sequence. Targets supply validated values and
assets but cannot select commands or executable paths.

## Shared hardware support

These statuses describe shared hardware validation. They do not qualify a
complete phone runtime closure for release; that state belongs in the target
document and `releases.lock.toml`.

| Block                 | Status         | Implementation                            | Notes                                                         |
| --------------------- | -------------- | ----------------------------------------- | ------------------------------------------------------------- |
| CPU                   | Supported      | `dts/ums9117.dtsi`                        | CPU0 is a Cortex-A7 at 1 GHz                                  |
| SMP                   | Not applicable | —                                         | The SoC has a single Cortex-A7 core                           |
| Interrupt controller  | Supported      | ARM GIC binding in `dts/ums9117.dtsi`     | SoC SPI numbers are shared; board drivers remain target-owned |
| System counter        | Supported      | `kernel/ums9117-timer.c`                  | 1 kHz UMS9117 counter                                         |
| Pike2 timer           | Supported      | Linux Spreadtrum timer driver integration | Uses the shared 32.768 kHz clock                              |
| Clock controller      | Partial        | Fixed clock nodes                         | No general UMS9117 clock-controller driver                    |
| USB device controller | Partial        | `kernel/ums9117-musb.c`                   | PIO gadget with 512-byte TX/RX FIFOs on EP1 and EP2           |
| USB host controller   | Not supported  | —                                         | No host-mode initialization path                              |
| GPIO / pin control    | Not supported  | —                                         | Board drivers currently use known MMIO state directly         |
| UART                  | Not supported  | —                                         | No platform UART driver or DTS node                           |
| MMC / SD controller   | Not supported  | —                                         | The SDIO0 host lives in the board target, not here            |
| DMA                   | Not supported  | —                                         | USB is deliberately PIO-only                                  |
| Audio                 | Not supported  | —                                         | No shared audio controller implementation                     |
| SPI / I2C             | Not supported  | —                                         | No generic bus-controller nodes or drivers                    |
| Watchdog / reset      | Not supported  | —                                         | Reboot and power-off support are not implemented              |

## DTS contract for targets

The platform DTSI defines CPU0, GIC, fixed clocks, timers and the MUSB
controller. The USB node is disabled by default. A qualified target enables it
with an override such as:

```dts
&usb {
    dr_mode = "peripheral";
    status = "okay";
};
```

The override is valid only when that phone's bootstrap and wiring provide the
required inherited state. Targets add board devices under their own DTS and do
not duplicate SoC-wide timer, GIC or USB addresses.

## Kernel integration

| Path                                 | Responsibility                                             |
| ------------------------------------ | ---------------------------------------------------------- |
| `kernel/0001-ums9117-platform.patch` | Registers the ARM machine, timers and MUSB glue with Linux |
| `kernel/Kconfig`                     | Defines `CONFIG_ARCH_UMS9117`                              |
| `kernel/Makefile`                    | Builds shared machine and timer objects                    |
| `kernel/ums9117-machine.c`           | ARM DT machine declaration                                 |
| `kernel/ums9117-timer.c`             | UMS9117 system counter                                     |
| `kernel/ums9117-musb.c`              | Inherited-state MUSB gadget glue                           |
| `dts/ums9117.dtsi`                   | Shared CPU, GIC, clocks, timers and USB nodes              |

## Known constraints

- The current platform describes one CPU and does not bring up secondary cores.
- MUSB support assumes that the target bootstrap has already prepared the
  controller and PHY. It does not perform a complete cold initialization.
- USB runs in PIO gadget mode; DMA and host mode are not implemented. The FIFO
  table provides EP1 and EP2 bulk pairs, and the fixed host adapter attaches the
  shell on data interface 0 after Linux takes over USB.
- Clock, reset, pin-control and power-domain frameworks are not implemented;
  controller support that depends on them is unavailable.

## Targets using this platform

| Target                                                 | Phone                   | Enabled shared blocks             |
| ------------------------------------------------------ | ----------------------- | --------------------------------- |
| [`nokia-ta1618`](../../targets/nokia-ta1618/README.md) | Nokia 3210 4G (TA-1618) | CPU0, GIC, timers and MUSB gadget |

A shared block is marked **Supported** when a listed phone target has validated
it on physical hardware and the implementation is present. See the TA-1618
target document for its current limitations and exact release-qualification
state.
