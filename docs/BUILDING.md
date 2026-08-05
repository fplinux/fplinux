# Building FPLinux

FPLinux builds the complete phone image from source and pinned upstream inputs.
Generated files remain outside the source tree.

## Requirements

- Linux x86-64
- rootless Podman
- Python 3.11 or newer
- network access for the first build, or for the first source check when pinned
  inputs are not cached

Check the build host:

```sh
./fplinux doctor
```

`doctor` checks the host architecture, Podman rootless mode and whether the one
pinned OCI build image matches the current toolchain recipe. It does not check
phone runtime libraries or USB access.

The source-quality gate is optional for ordinary build and run use:

```sh
./fplinux check
```

Run it when changing or reviewing source. `check` uses the same pinned OCI
environment as the build. It runs Prettier and markdownlint-cli2 for Markdown,
Prettier for JSON, Taplo for TOML, Vale and typos for prose, gitleaks for
secrets, REUSE for licensing
metadata, Ruff and mypy for Python, shell checks, Buildroot `check-package` for
`buildroot-external` files, hadolint for the toolchain Containerfile,
clang-format for userspace and bootstrap C style, Clang `scan-build` for
userspace C, and the pinned kernel tree's own tooling for kernel sources: its
clang-format style, `checkpatch.pl`, the canonical `savedefconfig` form, device
tree schema validation through `dtbs_check`, and `sparse`. The sparse phase prepares the target's pinned Linux integration tree, Kconfig
and generated headers instead of checking drivers against substitute headers.
Source snapshots are mounted read-only and quality and analysis output stays
under `.cache/`. After its pinned inputs are downloaded, the analysis runs
without network access. If the OCI environment is missing or stale, `check`
rebuilds the same image tag and removes the replaced image ID unless an existing
container still references it.

The first build creates the pinned OCI environment automatically. It can also be
prepared explicitly:

```sh
./fplinux setup
```

Rootless Podman requires subordinate UID and GID mappings in `/etc/subuid` and
`/etc/subgid`. Use the official [Podman installation guide](https://podman.io/docs/installation)
and [rootless-mode requirements](https://docs.podman.io/en/latest/markdown/podman.1.html#rootless-mode)
for distribution-specific setup. `./fplinux doctor` checks that Podman is
installed and running rootless.

For disk-space troubleshooting, the current Nokia TA-1618 build occupied about
6.84 GB on one build host. This observation is not a minimum requirement; use
the [cache recovery](#cache-cleanup-and-recovery) steps if generated data fills
the filesystem.

## Build a phone target

```sh
./fplinux build nokia-ta1618
```

Use `--jobs` to control parallel compilation:

```sh
./fplinux build nokia-ta1618 --jobs 8
```

The dispatcher auto-discovers `targets/<target>/target.toml`, validates the
`fplinux.target/v1` data against the selected `platform.toml` and runs
`scripts/fplinux_cli/builder.py` inside the one pinned OCI image. Targets do not
supply executable build hooks. After selecting a target, read its documentation
for hardware status and phone-specific constraints; for example,
[Nokia TA-1618](../targets/nokia-ta1618/README.md).

The shared builder performs four stages:

1. Buildroot creates the musl/BusyBox root filesystem from the platform's shared
   paths and the target defconfig.
2. Kbuild projects the platform and target Linux patches, copies and appends,
   then builds Linux with the initramfs and target DTB.
3. The bootstrap stage projects the platform-declared pinned vendor files,
   combines the zImage and DTB into `ramboot.bin` and checks the generic
   RAM-only image contract.
4. Typed platform recipes build the host tools, the generic
   `fplinux.assets/v1` lock resolves target assets, and the builder publishes the
   shared runner, fixed platform adapter and deterministic target bundle.

## Build layout

```text
.cache/
├── analysis/scan-build/<recipe>/              userspace analyzer reports
├── analysis/sparse/<target>/<recipe-config>/  sparse Kbuild output
├── downloads/                                pinned upstream downloads
├── linux/sources/<target>/                   current Linux integration tree
├── quality-workspaces/<recipe>/              read-only source-quality input
├── workspaces/<recipe>/                      immutable target build input
└── out/<target>/
    ├── work/
    │   ├── assets/                 verified extracted board assets
    │   ├── buildroot/              Buildroot O= directory
    │   ├── kernel-<recipe>/        Kbuild O= directory
    │   ├── bootstrap/              bootstrap projection and objects
    │   ├── host-build/             host-tool source and objects
    │   └── host/                   built host tools
    └── <profile>/                  runnable target bundle
```

The workspace recipe hashes only the selected target closure: the shared
builder and validators, the selected target and release/asset manifests, target
sources, the selected platform declaration and sources, and referenced common
inputs. The immutable workspace carries that recipe in `.fplinux-workspace`;
the successful `build-manifest.json` records the same receipt and the generated
bundle hashes. Linux objects never enter the prepared Linux source tree. The
cache keeps one recipe-validated Linux tree per target: a changed recipe is
prepared completely before it replaces that target's previous tree. Failed
preparation removes its staging directory. The container receives only cache
and generated-output mounts as writable; the workspace is read-only.

## Cache cleanup and recovery

Stop any active `build`, `check` or `package` command before removing cache
paths. Everything under `.cache/` is generated or downloaded; deleting it does
not modify source files.

To discard one target's build products while retaining downloaded inputs and the
prepared Linux tree:

```sh
rm -rf -- .cache/out/nokia-ta1618
```

This removes the runnable bundle and build receipt. `run` and `package` will
reject the missing output until `./fplinux build nokia-ta1618` succeeds again.

To reclaim all project cache space and force a complete local rebuild:

```sh
rm -rf -- .cache
```

The next build recreates the cache, downloads missing pinned inputs and rebuilds
the target. The tagged Podman image is stored outside `.cache/`, so deleting the
cache does not remove it. `setup`, `build` and `check` reuse that image when its
recipe still matches; otherwise setup replaces it. Preparation errors clean their
staging directory without replacing the target's current Linux source tree.

For Nokia TA-1618 the runnable output is:

```text
.cache/out/nokia-ta1618/console/
```

## Important outputs

```text
image/ramboot.bin                 complete RAM-boot image
host/spd_dump                     BootROM/FDL transport
host/libc_server                  bootstrap display/USB server
host/fplinux-usb-console          Linux USB serial client
runner/run.py                     shared RAM-only runner
runner/platform_adapter.py        fixed platform host adapter
runtime-manifest.json             generic runtime contract and hashes
assets.lock.toml                  generic fplinux.assets/v1 source lock
assets/                           pinned target boot assets
debug/                            kernel and image inspection artifacts
build-manifest.json               workspace receipt and bundle file hashes
```

`debug/` supports local inspection and is never included in a user release
archive. The build validates generic runtime fields in `target.toml` and the
asset lock, then emits only the addresses, USB metadata, adapter data,
role-based paths and file hashes needed by the shared runner into deterministic
`runtime-manifest.json`. The fixed platform adapter validates the exact adapter
keys and ranges before it performs any host or USB operation.

## Build receipts and hardware qualification

A successful build writes deterministic inspection outputs under `debug/` and a
`build-manifest.json` receipt for the selected source workspace, toolchain recipe
and generated bundle hashes. This proves which source closure created the bundle;
it does not prove that the bundle works on a phone.

Hardware qualification is a separate phone-side gate over the exact runtime
closure. Feature-level hardware status is recorded in the target README. A
qualified release additionally requires the exact closure digest in
`releases.lock.toml`.

## Package a build

Create a package for physical qualification:

```sh
./fplinux package nokia-ta1618 --candidate
```

Packaging requires an existing successful build with a matching
`build-manifest.json`. It rejects stale, mixed or modified runtime files, writes
a deterministic ZIP under `.cache/out/candidates/` and uses the data-only
`fplinux.release/v1` allowlist. It does not rebuild the target.

The source tree has no qualified runtime closure or prebuilt archive. After an
exact candidate passes the phone gate, maintainers record its printed runtime
SHA-256 in `releases.lock.toml` and package without `--candidate`. Release
archives are written under `.cache/out/releases/`. See
[Release archives](RELEASES.md) for the qualification and archive contract.

## Reproducibility

The toolchain recipe, Linux, Buildroot, downloaded source archives and phone
assets are version- and SHA-256-pinned. The source tree contains one
Containerfile and creates one tagged FPLinux OCI environment; the digest-pinned
Debian parent is pulled, not built by this project. The local toolchain tag is
accepted only when its embedded recipe digest matches every file under
`toolchains/`. Build timestamps and Kbuild identity are fixed.
Packaging verifies the successful-build manifest, sorts entries, normalizes
timestamps and stores only the allowlisted runtime closure.

A matching build proves that the source assembles into the same bytes. A feature
is marked supported only after the resulting image is run on the corresponding
phone variant.
