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

## Current platforms

| Platform  | SoC                   | Shared support                                    | Targets                                             | Documentation                          |
| --------- | --------------------- | ------------------------------------------------- | --------------------------------------------------- | -------------------------------------- |
| `ums9117` | Unisoc UMS9117 / T117 | Cortex-A7, GIC, timers and MUSB gadget controller | [`nokia-ta1618`](../targets/nokia-ta1618/README.md) | [Platform document](ums9117/README.md) |

## Adding a platform

1. Copy the [platform template](../docs/porting/PLATFORM.md) to
   `<platform>/README.md` and fill it in.
2. Add `<platform>/platform.toml` with required `[rootfs].packages`,
   Linux/Kbuild integration, bootstrap vendor projection, typed host recipes and
   the runner contract.
3. Put reusable controller drivers and the SoC DTSI in the platform directory.
   Expose disabled-by-default DTS nodes; each target decides which devices are
   wired and enabled.
4. Add one fixed `host/adapter.py` that accepts only validated runtime data and
   implements the platform's RAM-only sequence. Select `common/run.py` as the
   shared runner.
5. Declare shared files and patches in `platform.toml`; targets declare only
   board-specific additions in their data-only `target.toml` files.
6. Add the platform to this index.

The fixed common rootfs packages are `fplinux-base`, `fplinux-console`,
`fplinux-input` and `fplinux-tyrquake`. A platform's required
`[rootfs].packages` array owns hardware-family additions; each target has its
own required array for board additions. The final set is their exact union and
does not depend on the target name. Targets with the same final set share a
rootfs.

Do not require targets to copy a build script, runner or launcher. Move code into
a platform only when its addresses, interrupts and behavior are SoC-wide.
Similar-looking board hardware is not enough.
