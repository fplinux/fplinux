# FPLinux

FPLinux is a source-built Linux port for selected feature phones. It loads into
volatile RAM alongside the vendor firmware; the supported loader workflows do
not flash, erase, partition, or modify phone storage.

## Supported targets

| Target               | Device                  | Platform                                 | Profile   | Hardware status                                              |
| -------------------- | ----------------------- | ---------------------------------------- | --------- | ------------------------------------------------------------ |
| `inoi-240-modern-4g` | INOI 240 Modern 4G      | [`ums9117`](platforms/ums9117/README.md) | `console` | [Target documentation](targets/inoi-240-modern-4g/README.md) |
| `inoi-244-modern-4g` | INOI 244 Modern 4G      | [`ums9117`](platforms/ums9117/README.md) | `console` | [Target documentation](targets/inoi-244-modern-4g/README.md) |
| `nokia-ta1618`       | Nokia 3210 4G (TA-1618) | [`ums9117`](platforms/ums9117/README.md) | `console` | [Target documentation](targets/nokia-ta1618/README.md)       |

Each target document is the source of truth for its tested phone variant,
boot-key instructions, available hardware, and limitations.

## Quick start

Build hosts need Linux x86-64, rootless Podman, and Python 3.11 or newer.
Network access is needed until the pinned build inputs are available locally.

```sh
./fplinux doctor
./fplinux check
./fplinux build <target>
```

`check` is recommended when changing or reviewing source; it is not required
before every ordinary build. See [Building FPLinux](docs/BUILDING.md) for setup,
offline builds, logs, cache use, and cleanup.

To load a built image, first power the phone off and disconnect USB. Start the
loader for the exact target **before** connecting the phone:

```sh
./fplinux run <target>
```

Wait until the loader requests the device, then connect the phone and follow the
boot-key instructions in that target's document. USB detection is diagnostic;
it does not select a target. The loader writes only volatile RAM. To reconnect to
a running console, use `./fplinux console <target>` rather than starting another
RAM load.

## Documentation

- [Building FPLinux](docs/BUILDING.md): host setup, builds, checks, cache, and
  source-build boundaries.
- [Release archives](docs/RELEASES.md): candidate archives, qualification,
  USB access, and archive troubleshooting.
- [Host-to-phone transfer](docs/TRANSFER.md): console commands and file copy.
- [Installable applications](docs/APPLICATIONS.md): APK installation and
  removal in a source-checkout RAM session.
- [Phone targets](targets/README.md): target index and per-phone documents.
- [Hardware platforms](platforms/README.md): reusable SoC support.
- [Porting FPLinux](docs/porting/README.md): contributor-facing porting
  contracts.

## Architecture

The repository separates shared Alpine userspace, pre-Linux bootstrap code,
host tooling, reusable SoC support, and phone-owned board support. Platform and
target manifests select standard-rootfs packages separately from installable
APKs published beside the image. Phone-specific addresses, panel setup, keymaps,
and hardware status remain with the target.

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
