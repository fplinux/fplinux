# {Manufacturer} {Model} ({hardware variant}) support

<!-- Copy this file to targets/<target>/README.md and replace every placeholder. -->

## Device

| Field             | Value                                      |
| ----------------- | ------------------------------------------ |
| Manufacturer      | `<manufacturer>`                           |
| Model             | `<marketing name>`                         |
| Hardware variant  | `<model code / revision>`                  |
| SoC               | [`<soc>`](../../platforms/<soc>/README.md) |
| CPU used by Linux | `<architecture, cores, clock>`             |
| FPLinux profile   | `<profile>`                                |
| Boot method       | `<RAM / storage / other>`                  |
| Linux version     | `<version>`                                |

## Current status

In one paragraph, describe what the current source implements and how a user
interacts with it. State current limitations and whether an exact runtime closure
is recorded for this target in `releases.lock.toml`. Keep this section limited to
the current source and runtime state.

## Target definition

Start the target with this data-only `target.toml` and replace the placeholders:

```toml
schema = "fplinux.target/v1"
name = "<target>"
display_name = "<manufacturer> <model> (<variant>)"
release_slug = "FPLinux-<manufacturer>-<model>-<variant>"
platform = "<platform>"
profile = "<profile>"
release_manifest = "release/manifest.toml"
assets_lock = "loader/assets.lock.toml"

[buildroot]
defconfig = "rootfs/defconfig"

[linux]
defconfig = "kernel/defconfig"
dtb = "<vendor>/<target>.dtb"
debug_dtb = "<target>.dtb"
patches = ["kernel/0001-<target>-board.patch"]
copies = [
  { source = "dts/<target>.dts", destination = "arch/<arch>/boot/dts/<vendor>/<target>.dts" },
]
appends = []
forbidden_config = ["CONFIG_<UNSAFE_STORAGE_PATH>=y"]
forbidden_dtb_markers = ["<marker>", ...]

[bootstrap]
source = "bootstrap"
image = "<target>_linux_bootstrap.bin"
map = "obj/<target>_linux_bootstrap_map.txt"
kernel_destination = "zImage"
dtb_destination = "<target>.dtb"
load_address = 0x00000000
payload_limit = 0x00000000
toolchain = "<toolchain>"
lto = 0

[runtime]
fdl1_load_address = 0x00000000
assets = { fdl1 = "assets/<fdl1>.bin" }

[runtime.adapter]
<platform_setting> = "<value>"

[runtime.usb.bootrom]
vendor_id = 0x0000
product_id = 0x0000
wait_seconds = 300

[runtime.usb.linux_console]
vendor_id = 0x0000
product_id = 0x0000
wait_seconds = 60
```

Use the real RAM load addresses and USB identifiers. The selected platform's
fixed adapter defines the exact keys and types accepted under
`[runtime.adapter]`; the target supplies values, not executable behavior. The
build validates the target and writes only the generic runtime contract required
by `common/run.py` to `runtime-manifest.json`; do not package the raw target
configuration.

A normalized directory containing a valid `target.toml` is auto-discovered. Do
not add a registry entry or copy a target `build.py`, runner or launcher. The
root command combines this manifest with `platforms/<platform>/platform.toml`
and invokes the shared `scripts/fplinux_cli/builder.py`.

## Asset and package data

Use a generic `fplinux.assets/v1` lock at the declared `assets_lock` path. Each
source states its kind, pinned URL and SHA-256; each output assigns a runtime
role, bundle path and expected SHA-256. Keep extraction or download behavior in
the shared builder rather than target scripts.

The declared release manifest uses `fplinux.release/v2` and contains exactly
`schema`, `image`, `bundle_files`, `runtime_files` and `documents`. `bundle_files`
is the complete generated bundle allowlist. `runtime_files` is its
hardware-qualified subset and must contain the image, every runtime asset, the
shared runner, host tools, platform adapter and `runtime-manifest.json`; exclude
build receipts, provenance and documentation. `documents` names target files
below `release/` that packaging adds to the archive. Executable roles come from
the selected platform's typed host recipes and shared runner, not from
target-controlled flags or launchers.

## Hardware support

Use these feature-level status values consistently:

- **Supported:** exercised on the named physical hardware variant, with the
  implementation present in the current target.
- **Partial:** exercised with the stated limitation or qualification gap.
- **Not supported:** not implemented or deliberately disabled.
- **Unknown:** hardware exists, but its state has not been established.
- **Not present:** this phone variant does not contain the hardware.

These values do not qualify an aggregate release. A qualified release requires
the complete exact runtime closure to pass the phone gate and have its digest in
`releases.lock.toml`.

| Area                             | Status     | Notes                                  |
| -------------------------------- | ---------- | -------------------------------------- |
| Boot                             | `<status>` | `<loader and persistence behavior>`    |
| Linux kernel and root filesystem | `<status>` | `<versions/profile>`                   |
| CPU / SMP                        | `<status>` | `<active cores and frequency>`         |
| Interrupt controller             | `<status>` | `<driver>`                             |
| Timers                           | `<status>` | `<clocksource/clockevent>`             |
| Display                          | `<status>` | `<framebuffer/DRM, mode, limitations>` |
| Keypad / keyboard                | `<status>` | `<input path>`                         |
| Touchscreen                      | `<status>` | `<or Not present>`                     |
| USB device mode                  | `<status>` | `<functions and USB IDs>`              |
| USB host mode                    | `<status>` | `<limitations>`                        |
| microSD / MMC                    | `<status>` | `<read/write and tested media>`        |
| Internal flash                   | `<status>` | `<read/write policy>`                  |
| Audio output                     | `<status>` | `<speaker/headphones>`                 |
| Microphone                       | `<status>` | `<capture path>`                       |
| Modem / calls / SMS / data       | `<status>` | `<available interfaces>`               |
| Bluetooth                        | `<status>` | `<controller/firmware>`                |
| Wi-Fi                            | `<status>` | `<controller/firmware>`                |
| Battery / charging               | `<status>` | `<power-supply reporting/control>`     |
| Suspend / power-off / reboot     | `<status>` | `<known behavior>`                     |
| Camera                           | `<status>` | `<sensor and pipeline>`                |
| LEDs / vibration                 | `<status>` | `<available controls>`                 |
| RTC                              | `<status>` | `<time retention>`                     |

Keep rows for hardware that is present, even when its current state is **Unknown**
or **Not supported**. Delete only rows for categories that do not apply after the
target document already makes the absence clear.

## Build

```sh
./fplinux doctor
./fplinux build <target>
```

Output:

```text
.cache/out/<target>/<profile>/
```

List any target-specific host requirement.

## Boot

```sh
./fplinux run <target>
```

Document whether the operation is RAM-only or persistent, the expected USB
IDs, required physical actions and how to reach a console. If the target exposes
multiple USB data interfaces, assign and document each interface number. When
the target supports the build stamp check, include:

```sh
./fplinux verify <target>
```

State that this compares the running image with the local build receipt rather
than qualifying hardware. Never hide a storage write inside a generic “run”
instruction.

## Target-specific nuances

- `<inherited bootstrap or vendor state>`
- `<memory-map and payload limits>`
- `<required firmware or generated assets>`
- `<known cold-boot/reboot behavior>`
- `<hardware revision differences>`

## Release qualification

State whether a prebuilt archive is currently available and whether this target
has a matching entry in `releases.lock.toml`. If it does, identify the exact
runtime closure digest and hardware variant. Do not list a candidate as a
release.

The generic [release contract](../RELEASES.md) defines candidate marking,
archive contents and release requirements.

## Driver configuration

Drivers required for the phone profile use built-in kernel configuration (`=y`).
Document any intentionally optional module and include it in the package contract
when the profile depends on it. A target that exposes the host keyboard bridge
also documents `CONFIG_INPUT_MISC=y`, `CONFIG_INPUT_UINPUT=y`, the gadget serial
port assigned to input and the init path that starts its receiver.

## Implementation map

| Path          | Responsibility                                                    |
| ------------- | ----------------------------------------------------------------- |
| `target.toml` | Data-only board inputs, identity and runtime values               |
| `dts/`        | Board DTS and phone wiring                                        |
| `kernel/`     | Phone-specific Linux support and integration fragments            |
| `bootstrap/`  | Target payload source and Linux handoff                           |
| `rootfs/`     | Target identity and files layered over the common root filesystem |
| `loader/`     | Generic asset lock and asset provenance                           |
| `release/`    | Data-only package allowlist and archive documents                 |

Delete rows for directories the target does not need. Link back to the shared
platform document at the end. Build, package and run behavior stays in the
shared builder, packager, runner and the selected platform adapter.
