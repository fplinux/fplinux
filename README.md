# FPLinux

FPLinux is a source-built Linux port for selected feature phones. It loads into
volatile RAM alongside the vendor firmware. The supported loader never flashes,
erases, partitions, or writes the phone's internal storage. A target may expose
removable media separately; only the explicitly documented workflows may write
such media, and they require a safe unmount before removal or shutdown.

## Supported targets

| Target               | Device                  | Platform                                 | Hardware status                                              |
| -------------------- | ----------------------- | ---------------------------------------- | ------------------------------------------------------------ |
| `inoi-240-modern-4g` | INOI 240 Modern 4G      | [`ums9117`](platforms/ums9117/README.md) | [Target documentation](targets/inoi-240-modern-4g/README.md) |
| `inoi-244-modern-4g` | INOI 244 Modern 4G      | [`ums9117`](platforms/ums9117/README.md) | [Target documentation](targets/inoi-244-modern-4g/README.md) |
| `nokia-ta1618`       | Nokia 3210 4G (TA-1618) | [`ums9117`](platforms/ums9117/README.md) | [Target documentation](targets/nokia-ta1618/README.md)       |

Target manifests own machine identity. Each target document owns the exact
phone's boot key, hardware support, safe use, and limitations; see the shared
[identity contract](docs/reference/IDENTITY.md).

## Documentation

### Quick start

Build hosts need Linux x86-64 and Python 3.14. FPLinux installs its
pinned Kern binary inside the project cache; no system container engine is
required. Network access is needed until the pinned build inputs have been
stored locally.

```sh
./fplinux setup
./fplinux doctor
./fplinux build <target>
```

See [Building FPLinux](docs/guides/BUILDING.md) for setup, source checks, offline
builds, logs, cache use, and cleanup.

### Guides

First choose the exact phone in [Phone targets](targets/README.md) and read its
support status, boot key, storage rules and limitations.

- [Building FPLinux](docs/guides/BUILDING.md)
- [Loading from a source checkout](docs/guides/LOADING.md)
- [Using a standalone archive](docs/guides/STANDALONE.md)
- [Release archives](docs/guides/RELEASES.md)
- [Hardware debugging](docs/guides/DEBUGGING.md)

### Features

- [Local console](docs/features/LOCAL_CONSOLE.md)
- [USB networking](docs/features/USB_NETWORKING.md)
- [SSH access](docs/features/SSH.md)
- [File transfer](docs/features/FILE_TRANSFER.md)
- [Host keyboard forwarding](docs/features/HOST_KEYBOARD.md)
- [CPU clock reporting](docs/features/CPU_CLOCK.md)

### Applications

- [TyrQuake](docs/apps/TYRQUAKE.md)
- [MicroPythonOS](docs/apps/MICROPYTHONOS.md)

### Reference

- [C code](docs/reference/C_STYLE.md)
- [Target and platform identity](docs/reference/IDENTITY.md)
- [Logging contract](docs/reference/LOGGING.md)

Before submitting source changes, run the complete uncached quality gate from
[Building FPLinux](docs/guides/BUILDING.md#check-source).

### Porting

- [Porting overview](docs/porting/README.md)
- [Phone target contract](docs/porting/TARGET.md)
- [Platform contract](docs/porting/PLATFORM.md)
- [Console contract](docs/porting/CONSOLE.md)

## Architecture

The repository separates [Alpine userspace](alpine/README.md),
[shared pre-Linux components](bootstrap/README.md), the
[shared host and runtime stack](common/README.md), reusable
[SoC platforms](platforms/README.md), and phone-owned
[targets](targets/README.md). Platform and target manifests select standard
rootfs packages separately from installable APKs published beside the image.
Phone-specific addresses, panel setup, keymaps, and hardware status remain with
the target.

## Provenance

FPLinux is an independent reverse-engineering project. The repository contains
no vendor firmware, vendor source, or manufacturer documentation. Hardware
claims in target documents distinguish physical-device observations from
source-build and upstream evidence. Release qualification is recorded
separately for one exact executable payload.

## License

Original FPLinux code and documentation are licensed under
[GPL-2.0-only](LICENSE) unless a file says otherwise. Downloaded and third-party
components retain their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
