# Phone targets

A target describes one concrete phone variant. Its
`fplinux.target/v1` manifest and board sources own everything that changes
between phones using the same SoC:

- model and compatible strings;
- board memory map and reserved regions;
- enabled peripherals and board wiring;
- display panel, keypad matrix and other phone-specific drivers;
- bootstrap source, configuration and load addresses;
- root filesystem profile;
- generic asset-lock data and platform-adapter values;
- a data-only release allowlist and target archive documents;
- a `README.md` with the complete support matrix and device nuances.

Shared CPU, interrupt-controller, timer, controller integration, bootstrap vendor
projection, typed host recipes and the fixed host adapter belong under
[`platforms/`](../platforms/README.md). Stages 1–4 are implemented once in the
shared `scripts/fplinux_cli/builder.py`, and RAM execution starts in
`common/run.py`.

## Current targets

| Target         | Phone                   | Platform  | Profile   | Documentation                                  |
| -------------- | ----------------------- | --------- | --------- | ---------------------------------------------- |
| `nokia-ta1618` | Nokia 3210 4G (TA-1618) | `ums9117` | `console` | [Target documentation](nokia-ta1618/README.md) |

## Adding a target

1. Copy the [phone target template](../docs/porting/TARGET.md) to
   `<target>/README.md` and fill it in.
2. Add `<target>/target.toml` with schema `fplinux.target/v1`, phone/release
   identity, the platform, defconfigs, Linux and bootstrap inputs, runtime values
   and paths to the asset and release manifests.
3. Add only the board-specific DTS, drivers, bootstrap and rootfs pieces required
   by that phone. Keep SoC-wide code in `platforms/<soc>/`.
4. Describe downloaded assets with a generic `fplinux.assets/v1` lock. Describe
   package `bundle_files` and `documents` with a data-only
   `fplinux.release/v1` manifest.
5. Do not copy another target's `build.py`, runner or launcher. The root
   dispatcher auto-discovers normalized target directories containing
   `target.toml` and invokes the shared builder and runner contracts.
6. Add the phone to this index and the factual target index in the root README.

A target may start small. Mark unavailable hardware explicitly instead of
adding placeholder drivers or copying unrelated code from another phone.
