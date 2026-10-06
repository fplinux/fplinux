# Building FPLinux

FPLinux builds complete phone images from the current source checkout and
pinned upstream inputs. Generated build data is kept under `.cache/`, outside
tracked source files.

## Requirements and setup

- Linux x86-64
- Python 3.14
- unprivileged user namespaces
- `newuidmap` and `newgidmap` with subordinate UID and GID ranges
- network access until the pinned build inputs have been stored locally

Prepare the project-local Kern binary and pinned build environment, then check
the host:

```sh
./fplinux setup
./fplinux doctor
```

`setup` downloads the pinned Kern release into the project cache and builds the
pinned OCI environment. No system container engine is required. `doctor` checks
the host architecture, the exact project-local Kern binary, Kern's host
requirements, and the pinned build environment. Kern's image store,
configuration and downloaded binary remain under `.cache/`. Temporary box state
uses the current user's `$XDG_RUNTIME_DIR`.

The first build also prepares a missing environment automatically.

In a Git checkout, `setup` also selects the repository's commit-message hook.
Commits use `type(scope): subject`, followed by a blank line and a non-empty
explanatory body. The tracked commitlint configuration is the source of truth
for accepted scopes.

Use `./fplinux setup --force` to rebuild the pinned OCI image even when an image
for the current recipe is already ready.

## Build a target

Choose the exact phone in the [target index](../../targets/README.md), and read
its support status, boot key and storage limitations before building. Target
names are discovered from `targets/`.

```sh
./fplinux build <target>
./fplinux build <target> --jobs 8
```

Each build container starts with a 2 GiB memory budget.
Builds use up to eight available CPUs by default; `--jobs` selects an explicit
parallel compilation limit. Build processes run at nice level 10, preserving
an already lower scheduling priority so that foreground work can take precedence.
A matching selected bundle is reused;
otherwise the command rebuilds it from the current inputs.

After an online build has prepared the required inputs, an offline build miss
can run with networking disabled:

```sh
./fplinux build <target> --offline
```

If the required pinned environment is missing or stale, an offline build
asks for an online `./fplinux setup` first. A matching bundle remains usable
offline.

### Build types

`--build-type release` is the default. `--build-type debug` selects kernel
diagnostics without changing the userspace package set or peripheral support.
Build type and boot profile are independent selectors.

| Kernel facility                         | `release` | `debug`  |
| --------------------------------------- | --------- | -------- |
| Symbol names (`kallsyms`)               | Included  | Included |
| Kernel log buffer                       | 64 KiB    | 128 KiB  |
| Loadable modules                        | Disabled  | Enabled  |
| ftrace, kprobes, DMATEST and `/dev/mem` | Disabled  | Enabled  |

Use the same build type when building, loading, reconnecting, verifying,
packaging or inspecting a session:

```sh
./fplinux build <target> --build-type debug
./fplinux run <target> --build-type debug
./fplinux console <target> --build-type debug
./fplinux verify <target> --build-type debug
./fplinux check kernel --build-type debug
```

The UMS9117 kernel and embedded initramfs use XZ compression. Both build types
use the same userspace packages.

Each target, profile and build type has its own current bundle. Selecting one
does not replace another. Host debug files are separate from the phone payload;
see [artifact inspection](DEBUGGING.md#inspect-built-artifacts). A debug image is not a
hardware qualification.

### Two global profiles

FPLinux has two global profiles, declared once under `profiles/`:

- `default`: the system root is a writable RAM OverlayFS over a read-only
  XZ-compressed SquashFS. Omitting `--profile` and explicitly
  selecting `--profile default` use the same build and runtime identity.
  zram uses ZSTD compression.
- `microsd-uboot`: the USB loader starts U-Boot in RAM, then Linux uses the
  microSD card as its persistent ext4 root. zram uses LZO-RLE compression.

Both profiles use the shared platform configuration and the selected target's
board configuration. A target without microSD boot inputs offers only
`default`: selecting `microsd-uboot` for it fails, and a `microsd-uboot` kernel
check covers only the targets that offer that profile. A profile selects boot
and storage policy, not individual peripheral features. Board initialization,
panel geometry, keys and fitted firmware declarations remain target-owned.
Separate feature profiles are not accepted.

Build, check, load and inspect the selected context explicitly:

```sh
./fplinux check kernel --profile microsd-uboot
./fplinux build <target> --profile microsd-uboot
./fplinux package <target> --profile microsd-uboot --candidate
./fplinux run <target> --profile microsd-uboot
./fplinux console <target> --profile microsd-uboot
./fplinux verify <target> --profile microsd-uboot
```

For `run` and `package`, `--boot microsd` is an alias for
`--profile microsd-uboot`. Do not combine the two selectors. Neither selector
falls back to a different target or boot mode. Actual board support is recorded
in the [target index](../../targets/README.md); selecting a global profile does
not establish hardware support.

Default and microSD builds keep separate current bundles and work state. A
microSD build cannot replace the default bundle. Non-default archives currently
require `--candidate`; candidate packaging does not test the image on a phone.

The microSD build produces a partitioned `FPLINUX.img.xz`, containing a SHA-256
FIT on FAT32 and an ext4 system root. Building never writes removable media.
Follow [microSD system root](MICROSD_ROOT.md) for the layout, persistence and
shutdown rules.

### Local fitted device data

Phone-specific inputs are optional complete groups from the exact handset.
Normal builds consume prepared local data and do not read the phone's NAND.
See [Device data and NAND backups](DEVICE_DATA.md) for group requirements and
preparation before rebuilding.

After building, follow [Loading from a source checkout](LOADING.md) to configure
the host, load the selected image, reconnect and verify the running context.

## What a build proves

A successful build proves that the current checkout produced the selected
bundle. It does not prove that the image boots or that a hardware feature works
on a phone. Target documents record current feature support and limitations.

See [Release archives](RELEASES.md) to create a phone-test candidate. To use the
result on a phone, continue with
[Loading from a source checkout](LOADING.md).

## Format source

Use the [source formatting procedure](DEVELOPMENT.md#format-source).

## Check source

Use the [source quality gate](DEVELOPMENT.md#check-source) before submitting changes.

## Regenerate Alpine checksums

Use the [supported checksum command](DEVELOPMENT.md#regenerate-alpine-checksums).

## Create a new target

Follow [Create a headless target](../porting/NEW_PHONE.md#create-a-headless-target).

## Prepare device data

Follow [device-data preparation](DEVICE_DATA.md#prepare-device-data).

## Save a NAND backup

Follow the [read-only NAND backup procedure](DEVICE_DATA.md#save-a-nand-backup).

## Shared Linux sources

Follow the [shared Linux integration contract](../porting/LINUX_INTEGRATION.md).
