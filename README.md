# FPLinux

FPLinux ports Linux to feature phones. The project contains complete source ports,
reproducible build environments and phone-specific RAM loaders. Shared SoC
support is separate from each phone's board support.

FPLinux boots alongside the original phone software: the current boot paths
load into volatile RAM and do not replace the vendor firmware.

## Photos

Photographs are not included in this source snapshot.

<!-- Boot-screen photograph goes here. -->

<!-- Local phone-console photograph goes here. -->

## Phone targets

| Target         | Device                  | Platform                                 | Profile   | Documentation                                          |
| -------------- | ----------------------- | ---------------------------------------- | --------- | ------------------------------------------------------ |
| `nokia-ta1618` | Nokia 3210 4G (TA-1618) | [`ums9117`](platforms/ums9117/README.md) | `console` | [Target documentation](targets/nokia-ta1618/README.md) |

Hardware support status, qualification state and phone-specific limitations are
recorded only in the target documentation.

## Build from source

The source tree contains the kernel drivers, DTS files, bootstrap, root
filesystem configuration and pinned build environment needed to reproduce the
phone image.

Host requirements:

- Linux x86-64;
- rootless Podman;
- Python 3.11 or newer;
- network access for the first build, or for the first source check when pinned
  inputs are not cached.

Rootless Podman requires subordinate UID and GID mappings in `/etc/subuid` and
`/etc/subgid`. Follow the official [Podman installation guide](https://podman.io/docs/installation)
and [rootless-mode requirements](https://docs.podman.io/en/latest/markdown/podman.1.html#rootless-mode)
for your distribution. `./fplinux doctor` reports whether Podman is installed
and running rootless.

Build the current phone target:

```sh
./fplinux doctor
./fplinux build nokia-ta1618
```

`check` is not a build prerequisite. Run it when changing or reviewing source:

```sh
./fplinux check
```

It checks Markdown, JSON, TOML, documentation, licensing metadata, Python,
shell, Buildroot, container recipe and C sources in the pinned environment. The C passes use Clang `scan-build` for
userspace and `sparse` with the target's real Linux tree, Kconfig and generated
headers for kernel code. Source snapshots are mounted read-only, analyzer work
stays under `.cache/`, and analysis runs without network access after its pinned
inputs have been downloaded.

The complete build stays under `.cache/`; it does not write generated files
into the source tree. The runnable bundle is produced at:

```text
.cache/out/nokia-ta1618/console/
```

Targets are discovered from `targets/*/target.toml`. The data-only
`fplinux.target/v1` manifest selects a platform and board inputs; the command
dispatches stages 1–4 to the shared `scripts/fplinux_cli/builder.py`.

See [Building FPLinux](docs/BUILDING.md) for the cache layout, recovery steps and
pinned inputs.

## Run from the source checkout

The generated host tools run on Linux x86-64 and require Python 3.11 or newer,
glibc 2.38 or newer, libusb 1.0, libudev and GNU `stdbuf` from coreutils. USB
access must allow the current user to read and write the Nokia TA-1618 BootROM
and Linux console devices; install the [documented udev rule](docs/RELEASES.md#usb-access)
before connecting the phone.

Build the target, power the phone off and start the RAM loader before connecting
the phone:

```sh
./fplinux run nokia-ta1618
```

The runner validates host-library dependencies before asking for the phone. The
loader then tells you when to hold `*` and connect USB, verifies BootROM USB
access and loads FDL1 and the FPLinux image into RAM. It contains no flash,
erase, partition or NV operation.

In the attached console, `Ctrl-]` detaches the host client without stopping the
phone shell, rebooting Linux or powering the phone off. `Ctrl-C` is sent to the
phone shell. If Linux is already running, reconnect directly instead of starting
the BootROM loader again:

```sh
.cache/out/nokia-ta1618/console/host/fplinux-usb-console
```

To leave the RAM session, detach with `Ctrl-]`, disconnect USB, remove the
TA-1618 battery, then reinsert it. The next power-on uses the unchanged vendor
firmware because the FPLinux payload was held only in volatile RAM. Linux
`reboot`, `poweroff` and PMIC-controlled shutdown are not qualified exit paths.
See [Console lifecycle](docs/RELEASES.md#console-lifecycle) for the full boundary.

## Release archives

No prebuilt archive is currently available. A successful local build can be
packaged as a clearly named candidate:

```sh
./fplinux package nokia-ta1618 --candidate
```

The ZIP is written under `.cache/out/candidates/`. See
[Release archives](docs/RELEASES.md) for the package contents, current Linux host
requirements and the phone-validation boundary. Windows archives are not
available.

## Port architecture

```text
fplinux             repository CLI entry point
scripts/fplinux_cli shared validation, build and package orchestration
Containerfile       the one pinned OCI build environment
buildroot-external/ shared Buildroot integration and packages
bootstrap/          reusable freestanding pre-Linux UI primitives
common/             shared post-kernel userspace and the shared RAM runner
platforms/<soc>/    platform.toml, reusable SoC support and fixed host adapter
targets/<phone>/    data-only target, board sources, assets and release inputs
```

A target does not provide an executable build hook, runner or launcher. Its
`target.toml` is auto-discovered. The selected `platform.toml` declares
Linux/Kbuild integration, bootstrap vendor projection, typed host-tool recipes
and the runner API version. The builder packages the conventionally located
shared runner and fixed platform adapter; manifests cannot select executable
paths.

Drivers required by a phone profile are built into that phone's kernel image.
A target documents unavailable hardware explicitly instead of carrying
placeholder drivers.

Platform and phone indexes:

- [Phone targets](targets/README.md)
- [Hardware platforms](platforms/README.md)

Porting templates live under [`docs/porting/`](docs/porting/README.md).

## How this port was derived

FPLinux is an independent reverse-engineering project. The hardware behaviour it
depends on was derived from the device itself: firmware dumps, register probing
and instrumented boot runs on real hardware, together with the published source
of the [fpdoom](https://github.com/ilyakurdyukov/fpdoom) bare-metal port for the
same SoC family.

This repository contains no vendor source code, no firmware binary and no
manufacturer document. It carries no binary artifacts at all; the source quality
gate rejects them. The vendor download-mode loader and the board maps the build
needs are fetched at build time from their upstream public releases and are
never redistributed here.

Recorded facts are the observed ones. Where behaviour could not be confirmed on
hardware, the documentation says so instead of guessing, and the affected
feature stays marked as unqualified. The per-target documents carry those
status tables.

## License

Original FPLinux code and documentation are licensed under
[GPL-2.0-only](LICENSE) unless a file says otherwise. Downloaded and third-party
components retain their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
