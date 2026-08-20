# Hardware platforms

A platform contains support shared by multiple phones built around the same SoC
or SoC family. Its `platform.toml` and sources own reusable hardware blocks and
the contracts that targets consume:

- CPU and architecture integration;
- interrupt controllers;
- clocks and timers;
- common buses and controller drivers;
- SoC-level DTSI nodes;
- Kconfig and Kbuild integration shared by every phone using the platform;
- platform-owned rootfs package additions;
- bootstrap vendor projection and shared bootstrap inputs;
- typed host-tool build recipes and fixed runtime-tool roles;
- one fixed host adapter used by the shared `common/run.py`.

A platform must not contain a phone model, panel choice, keypad matrix, board
memory reservation or phone-specific loader asset. Those belong under
[`targets/`](../targets/README.md).

## Platforms

| Platform  | SoC                   | Shared support                                    | Targets                                                                                                                                                                               | Documentation                          |
| --------- | --------------------- | ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------- |
| `ums9117` | Unisoc UMS9117 / T117 | Cortex-A7, GIC, timers and MUSB gadget controller | [`inoi-240-modern-4g`](../targets/inoi-240-modern-4g/README.md), [`inoi-244-modern-4g`](../targets/inoi-244-modern-4g/README.md), [`nokia-ta1618`](../targets/nokia-ta1618/README.md) | [Platform document](ums9117/README.md) |

## Platform directory contract

A platform directory contains:

- `README.md` following the [platform template](../docs/porting/PLATFORM.md);
- `platform.toml` with required `[rootfs].packages`, Linux/Kbuild integration,
  bootstrap vendor projection, typed host recipes and the runner contract;
- reusable controller drivers and the SoC DTSI;
- disabled-by-default DTS nodes for hardware selected by targets;
- one fixed `host/adapter.py` that accepts only validated runtime data and
  implements the platform RAM-only sequence;
- shared files and patches declared by `platform.toml`;
- an entry in this platform index.

The fixed common rootfs packages are `fplinux-base`, `fplinux-console`,
`fplinux-input` and `fplinux-tyrquake`. A platform's required
`[rootfs].packages` array owns hardware-family additions; each target has its
own required array for board additions. The final set is their exact union, and
does not depend on the target name. Targets with the same final set share a
rootfs. Targets contain only board-specific additions and do not copy build
scripts, runners or launchers. Platform code is limited to addresses, interrupts
and behavior that are SoC-wide; similar-looking board hardware is not sufficient.
