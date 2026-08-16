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
pinned OCI build image matches the current container recipe. It does not check
phone runtime libraries or USB access.

The source-quality gate is optional for ordinary build and run use:

```sh
./fplinux check
```

With no positional arguments, `check` runs the complete gate. List the available
scopes or run a subset while working on one area:

```sh
./fplinux check --list
./fplinux check python
./fplinux check docs spelling
./fplinux check --no-cache
```

Multiple scopes form one deduplicated selection and always run in canonical
order. The available scopes are `repository`, `source`, `container`, `metadata`,
`docs`, `spelling`, `secrets`, `licenses`, `python`, `shell`, `buildroot`, `c`
and `kernel`. Each cacheable scope stores a successful result. It is reused only
when the source closure, checker commands, orchestration recipe and OCI image
identity match. Any mismatch runs the scope again. `--no-cache` runs every
selected cacheable scope even when its recorded inputs match.

Commit messages use `type(scope): subject`. The scope is mandatory and must be
one of `bootstrap`, `build`, `cli`, `console`, `deps`, `input`, `nokia-ta1618`,
`quality`, `release`, `repo`, `rootfs` or `ums9117`. Use the narrowest component
that owns the change, such as `feat(nokia-ta1618): ...` for phone-specific
hardware support and `build(quality): ...` for the source-quality gate.
`./fplinux setup` selects the tracked `.githooks/commit-msg` hook for the current
Git checkout. The hook validates each message in the pinned OCI environment
before Git creates the commit. Source archives have no local Git configuration
and skip hook setup.

Run `check` when changing or reviewing source. It uses the same pinned OCI
environment as the build. It runs Prettier and markdownlint-cli2 for Markdown,
Prettier for JSON, Taplo for TOML, Vale and typos for prose, gitleaks for
secrets, REUSE for licensing
metadata, Ruff and mypy for Python, shell checks, Buildroot `check-package` for
`buildroot-external` files, hadolint for the Containerfile,
clang-format with the pinned kernel tree's style for userspace and bootstrap C,
Clang `scan-build` for userspace C, and the pinned kernel tree's own tooling for
kernel sources: its clang-format style, `checkpatch.pl`, the canonical `savedefconfig` form, device
tree schema validation through `dtbs_check`, and `sparse`. The sparse phase prepares the target's pinned Linux integration tree, Kconfig
and generated headers instead of checking drivers against substitute headers.
Source snapshots are mounted read-only, while quality and analysis output stays
outside those snapshots under `.cache/`. Userspace analysis is invocation-local;
only the fixed Sparse state is retained. After its pinned inputs are
downloaded, the analysis runs without network access. If the OCI environment is missing or stale, `check`
rebuilds the same image tag and removes the replaced image ID unless an existing
container still references it.

`check` prints one status per stage and keeps complete subprocess output below
`.cache/logs/check/<run-id>/`. Its unified invocation record is
`.cache/logs/check/<run-id>/run.json`. On failure the command prints a bounded
diagnostic tail and the exact log path. Add `--verbose` to stream complete output
while retaining the same logs.

## Cache coordination

Commands coordinate mutable cache state with a project-wide lock. `build`,
`check` and `setup` take it exclusively; `package`, `run`, `console` and
`verify` take a shared lock while resolving their current bundle. `prune --apply`
also takes the exclusive lock. A conflicting invocation waits until the lock is
available. `check --list` and a prune dry run do not take this lock.

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
the [cache inventory](#cache-inventory) steps if generated
data fills the filesystem.

## Build a phone target

```sh
./fplinux build nokia-ta1618
```

Use `--jobs` to control parallel compilation:

```sh
./fplinux build nokia-ta1618 --jobs 8
```

Build output follows the same compact stage format as `check`. Complete logs are
stored below `.cache/logs/build/<target>/<run-id>/`; the unified invocation
record is `.cache/logs/build/<target>/<run-id>/run.json`. `--verbose` streams
them to the terminal as well.

Before preparing a build, the command looks for a current bundle whose recorded
workspace and OCI image inputs match. On a match it reports a cached result and
skips Podman and workspace staging. `--jobs` controls scheduling and does not
change artifact identity. A mismatch proceeds through the normal build stages.

The dispatcher auto-discovers `targets/<target>/target.toml`, validates the
`fplinux.target/v1` data against the selected `platform.toml` and runs
`scripts/fplinux_cli/builder.py` inside the one pinned OCI image. Targets do not
supply executable build hooks. After selecting a target, read its documentation
for hardware status and phone-specific constraints; for example,
[Nokia TA-1618](../targets/nokia-ta1618/README.md).

The shared builder performs five stages:

1. The toolchain stage builds or reuses the shared musl cross toolchain in
   `.cache/toolchains/<digest>`, keyed by the platform's
   `toolchain_defconfig`, the pinned Buildroot identity and the Buildroot patch
   tree. Every target of a platform, and every rebuild, reuses the same tree.
2. Buildroot creates the BusyBox root filesystem against that external
   toolchain from the platform's shared paths, its
   `toolchain_external_defconfig` fragment and the target defconfig. A causal
   receipt splits the shared base from per-package payloads: a local package
   source change rebuilds only that package in place, while configuration
   changes rebuild the tree with `make clean`. Compilations flow through a
   shared ccache under `.cache/ccache`.
3. Kbuild projects the platform and target Linux patches, copies and appends,
   then builds Linux with the initramfs and target DTB.
4. The bootstrap stage projects the platform-declared pinned vendor files,
   combines the zImage and DTB into `ramboot.bin` and checks the generic
   RAM-only image contract.
5. Typed platform recipes build the host tools, the generic
   `fplinux.assets/v1` lock resolves target assets, and the builder publishes the
   shared runner, fixed platform adapter and deterministic target bundle.

## Build layout

```text
.cache/
├── analysis/sparse/<target>/                  Sparse Kbuild state
├── check-results/<scope>/                     check success results
├── downloads/                                pinned upstream downloads
├── linux/sources/<target>/                   current Linux integration tree
├── logs/check/<run-id>/                      source-quality logs and run.json
├── logs/build/<target>/<run-id>/             target-build logs and run.json
├── quality-workspaces/<recipe>/              source-quality input
├── workspaces/<recipe>/                      target build input
└── out/<target>/
    ├── work/
    │   ├── assets/                 extracted board assets
    │   ├── buildroot/              Buildroot O= directory
    │   ├── kernel/                 Kbuild O= directory
    │   ├── bootstrap/              bootstrap projection and objects
    │   ├── host-build/             host-tool source and objects
    │   └── host/                   built host tools
    ├── bundles/<profile>/<generation>/  runnable bundle generations
    └── <profile>.current.json            selected current generation
```

The workspace recipe hashes only the selected target closure: the shared
builder and validators, the selected target and release/asset manifests, target
sources, the selected platform declaration and sources, and referenced common
inputs. The workspace and `build-manifest.json` record build inputs and bundle
file information. `run`, `package` and `verify` use the selected current bundle.
`package` and `verify` compare its recorded inputs with the local checkout; if
they report a missing or stale bundle, rebuild the target.

## Cache inventory

To inspect cache workspace candidates, use the read-only inventory:

```sh
./fplinux prune
./fplinux prune --json
```

The default reports candidates and logical/allocated byte counts. `--json` emits
the same inventory as JSON. To apply a freshly calculated inventory:

```sh
./fplinux prune --apply
./fplinux prune --apply --json
```

`prune --apply` takes the exclusive cache lock and removes disposable staged
snapshots from the newly calculated plan in `workspaces/` and
`quality-workspaces/`.

For Nokia TA-1618, `console.current.json` selects the current runnable bundle:

```text
.cache/out/nokia-ta1618/console.current.json
```

The selected generation is below:

```text
.cache/out/nokia-ta1618/bundles/console/<generation>/
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
`build-manifest.json` receipt for the selected source workspace, container recipe
and generated bundle hashes. This proves which source closure created the bundle;
it does not prove that the bundle works on a phone.

Hardware qualification is a separate phone-side gate over the exact runtime
closure. Feature-level hardware status is recorded in the target README. A
qualified release additionally requires the exact closure digest in
`releases.lock.toml`.

## Verify a running phone

After the current bundle has been loaded and interface 0 is free, compare its
build stamp with the local build receipt:

```sh
./fplinux verify nokia-ta1618
```

The command first refuses a local bundle whose workspace or OCI recipe no
longer matches the checkout. It then reads `/etc/fplinux-build` and `uname -r`
through USB interface 0, comparing the Buildroot recipe and the device-kernel
suffix with `build-manifest.json`. The suffix covers the prepared Linux, rootfs,
kernel configuration, DTB and bootstrap recipe. It does not verify the other
bundle files and is not a hardware qualification gate.

The same repository entrypoint owns direct access to an already running target:

```sh
./fplinux console nokia-ta1618
./fplinux console nokia-ta1618 --exec 'uname -r'
./fplinux console nokia-ta1618 --upload ./file.bin /tmp/file.bin
./fplinux console nokia-ta1618 --pull /tmp/file.bin ./file.bin
```

`--keyboard EVDEV` selects the target's input channel; the other modes select
the shell and transfer channel.

## Package a build

Create a package for physical qualification:

```sh
./fplinux package nokia-ta1618 --candidate
```

Packaging requires an existing build whose recorded inputs match the current
source and container recipes. It writes a ZIP under `.cache/out/candidates/` and
uses the data-only `fplinux.release/v2` allowlist. Only its `runtime_files`
subset enters the hardware-qualified runtime digest. Packaging does not rebuild
the target.

The source tree has no qualified runtime closure or prebuilt archive. After an
exact candidate passes the phone gate, maintainers record its printed runtime
SHA-256 in `releases.lock.toml` and package without `--candidate`. Release
archives are written under `.cache/out/releases/`. See
[Release archives](RELEASES.md) for the qualification and archive contract.

## Reproducibility

The container recipe, Linux, Buildroot, downloaded source archives and phone
assets are version- and SHA-256-pinned. The source tree contains one
Containerfile and creates one tagged FPLinux OCI environment; the digest-pinned
Debian parent is pulled, not built by this project. The local image tag is
accepted only when its embedded recipe digest matches every recipe input:
`Containerfile`, `container.lock.toml`, the three package lock files and the
build driver modules. Build timestamps and Kbuild
identity are fixed.
Packaging verifies the successful-build manifest, sorts entries, normalizes
timestamps and includes the allowlisted bundle files, build receipt, target
and fixed legal documents, checksums and candidate notice when applicable.

A successful build records its selected source and build inputs. A feature is
marked supported only after the resulting image is run on the corresponding
phone variant.
