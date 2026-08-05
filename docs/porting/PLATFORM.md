# <SoC / platform name>

<!-- Copy this file to platforms/<platform>/README.md and replace placeholders. -->

## Identity

| Field                   | Value                               |
| ----------------------- | ----------------------------------- |
| Vendor                  | `<vendor>`                          |
| SoC / family            | `<part numbers>`                    |
| Architecture            | `<architecture>`                    |
| Linux platform symbol   | `<CONFIG_ARCH_...>`                 |
| DTS compatible          | `<vendor,soc>`                      |
| Reference documentation | `<public links or provenance note>` |

## Scope

Describe what this platform supplies to phone targets and what remains
board-specific.

## Platform layout

Start with only the files the SoC needs:

```text
platforms/<platform>/
├── README.md
├── platform.toml
├── dts/
│   └── <soc>.dtsi
├── host/
│   └── adapter.py
└── kernel/
    ├── Kconfig
    ├── Makefile
    ├── 0001-<platform>-integration.patch
    └── <shared-driver>.c
```

Delete unused entries rather than creating empty placeholders. Declare every
shared file and patch in `platform.toml`; targets add only their board-specific
Linux projections in `target.toml`.

## Declarative build and host contract

`platform.toml` describes the reusable mechanics consumed by the shared builder:

- `[buildroot]` selects the external tree, common rootfs paths and clean targets;
- `[linux]` declares the source lock, architecture, build and analysis compiler
  prefixes, Kbuild targets, integration patches and source projections;
- `[bootstrap]` declares the pinned vendor projection, shared copies, safety
  target and build targets;
- `[host]` declares a capability, fixed runtime-tool roles and typed recipes such
  as `make-archive/v1` and `cc-libusb/v1`;
- `[runner]` declares the platform-adapter API version. The shared builder always
  packages `common/run.py` and the conventionally located fixed adapter at
  `platforms/<platform>/host/adapter.py`; the manifest cannot select executable
  paths.

The adapter translates validated target runtime values into the platform's fixed
RAM-only sequence. It must validate its capability and data shape; target data
must not select commands or executable paths. Do not add target runners, copied
launchers or per-target build scripts.

## Shared hardware support

Use **Supported**, **Partial**, **Not supported**, and **Unknown** with the same
meaning as [`TARGET.md`](TARGET.md).

| Block                    | Status     | Driver / implementation | Target contract or limitation       |
| ------------------------ | ---------- | ----------------------- | ----------------------------------- |
| CPU / SMP                | `<status>` | `<path>`                | `<what targets configure>`          |
| Interrupt controller     | `<status>` | `<path>`                | `<interrupt numbering>`             |
| Clocksource / clockevent | `<status>` | `<path>`                | `<input clocks>`                    |
| Clock controller         | `<status>` | `<path>`                | `<fixed clocks or missing pieces>`  |
| GPIO / pin control       | `<status>` | `<path>`                | `<board pin assignment boundary>`   |
| UART                     | `<status>` | `<path>`                | `<available instances>`             |
| USB controller           | `<status>` | `<path>`                | `<host/device and inherited state>` |
| MMC / SD controller      | `<status>` | `<path>`                | `<instances and DMA constraints>`   |
| DMA                      | `<status>` | `<path>`                | `<addressing constraints>`          |
| Audio controller         | `<status>` | `<path>`                | `<codec remains target-specific>`   |
| SPI / I2C                | `<status>` | `<path>`                | `<available buses>`                 |
| Watchdog / reset         | `<status>` | `<path>`                | `<reboot/power behavior>`           |

## DTS contract for targets

List every label or node a target may reference:

| Label / compatible | Default state | Target responsibility           |
| ------------------ | ------------- | ------------------------------- |
| `<&controller>`    | `disabled`    | `<enable and add board wiring>` |

Targets must not duplicate SoC addresses or interrupt numbers already defined
here.

## Kernel integration

| Path              | Responsibility                                            |
| ----------------- | --------------------------------------------------------- |
| `platform.toml`   | Linux, bootstrap, typed host and runner declarations      |
| `kernel/Kconfig`  | Platform symbols and dependencies                         |
| `kernel/Makefile` | Shared platform objects                                   |
| `kernel/*.c`      | Reusable machine/controller support                       |
| `kernel/*.patch`  | Integration into upstream Linux                           |
| `dts/<soc>.dtsi`  | SoC blocks shared by targets                              |
| `host/adapter.py` | Fixed translation from runtime data to RAM-only host flow |

## Known constraints

- `<cold-init versus inherited firmware/bootstrap state>`
- `<address-width or DMA limitation>`
- `<unsupported controller modes>`
- `<required target-side setup>`

## Targets using this platform

| Target                                         | Phone     | Notes              |
| ---------------------------------------------- | --------- | ------------------ |
| [`<target>`](../../targets/<target>/README.md) | `<phone>` | `<enabled blocks>` |

A shared block becomes **Supported** when at least one listed phone target has
exercised it on physical hardware and the implementation is present in the
platform. This is feature-level hardware validation, not qualification of every
target's complete runtime closure. Link to each phone's current support document and
release state.
