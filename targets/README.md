# Phone targets

A target describes one concrete phone variant. Its
`fplinux.target/v1` manifest and board sources own everything that changes
between phones using the same SoC:

- model and compatible strings;
- board memory map and reserved regions;
- enabled peripherals and board wiring;
- display panel, keypad matrix and other phone-specific drivers;
- bootstrap source, configuration and load addresses;
- root filesystem profile and required target package additions;
- generic asset-lock data and platform-adapter values;
- a data-only release allowlist and target archive documents;
- a `README.md` with the complete support matrix and device nuances.

Shared CPU, interrupt-controller, timer, controller integration, bootstrap vendor
projection, typed host recipes and the fixed host adapter belong under
[`platforms/`](../platforms/README.md). Stages 1–4 are implemented once in the
shared `scripts/fplinux_cli/builder.py`, and RAM execution starts in
`common/run.py`.

## Targets

| Target               | Phone                   | Platform  | Profile   | Documentation                                        |
| -------------------- | ----------------------- | --------- | --------- | ---------------------------------------------------- |
| `inoi-240-modern-4g` | INOI 240 Modern 4G      | `ums9117` | `console` | [Target documentation](inoi-240-modern-4g/README.md) |
| `inoi-244-modern-4g` | INOI 244 Modern 4G      | `ums9117` | `console` | [Target documentation](inoi-244-modern-4g/README.md) |
| `nokia-ta1618`       | Nokia 3210 4G (TA-1618) | `ums9117` | `console` | [Target documentation](nokia-ta1618/README.md)       |

## Target directory contract

A target directory contains:

- `README.md` following the [phone target template](../docs/porting/TARGET.md);
- `target.toml` with schema `fplinux.target/v1`, phone/release identity, platform,
  required `[rootfs].packages`, defconfig, Linux and bootstrap inputs, runtime
  values and manifest paths;
- only board-specific DTS, drivers and bootstrap inputs required by that phone;
- a generic `fplinux.assets/v1` lock for downloaded assets;
- a data-only `fplinux.release/v2` manifest for `bundle_files`, qualified
  `runtime_files` and `documents`;
- entries in this index and the factual target index in the root README.

SoC-wide code belongs in `platforms/<soc>/`. Targets do not contain build scripts,
runners or launchers; the root dispatcher discovers directories containing
`target.toml` and invokes the shared builder and runner contracts. Unsupported
hardware is stated explicitly rather than represented by placeholder drivers.
The rootfs package set is the exact union of the fixed common packages
`fplinux-base`, `fplinux-console`, `fplinux-input` and `fplinux-tyrquake`, the
selected platform's required `[rootfs].packages` array and the target's required
array. The target name does not select packages. Targets with the same final set
and composition inputs share a rootfs.
