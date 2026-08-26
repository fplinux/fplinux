# Building FPLinux

FPLinux builds complete phone images from the current source checkout and
pinned upstream inputs. Generated build data is kept under `.cache/`, outside
tracked source files.

## Requirements and setup

- Linux x86-64
- rootless Podman
- Python 3.11 or newer
- network access until the pinned build inputs have been stored locally

Check the host before building:

```sh
./fplinux doctor
```

`doctor` checks the host architecture, rootless Podman, and the pinned build
environment. Rootless Podman needs subordinate UID and GID mappings; follow the
[Podman installation guide](https://podman.io/docs/installation) and
[rootless-mode requirements](https://docs.podman.io/en/latest/markdown/podman.1.html#rootless-mode)
for the host distribution.

The first build prepares its required environment automatically. It can also be
prepared explicitly:

```sh
./fplinux setup
```

In a Git checkout, `setup` also selects the repository's commit-message hook.
Commits use `type(scope): subject`, followed by a blank line and a non-empty
explanatory body. The tracked commitlint configuration is the source of truth
for accepted scopes.

Use `./fplinux setup --force` to rebuild the pinned OCI image even when an image
for the current recipe is already ready.

## Check source

Run the complete uncached source-quality gate before committing or submitting
source changes:

```sh
./fplinux check --no-cache
./fplinux check --list
./fplinux check docs spelling
```

With no scopes, `check` runs the complete gate. Selected cacheable scopes reuse
an exact successful result when their current inputs match; otherwise they run
again. `--no-cache` reruns selected cacheable scopes. An ordinary build or RAM
run without source changes does not need to repeat the gate.

The `docs` scope also rejects repository-local Markdown links whose file or
heading anchor does not exist.

Kernel, bootstrap, host and phone-userspace messages follow the shared
[logging contract](../reference/LOGGING.md). FPLinux-owned C follows
[C code](../reference/C_STYLE.md).

## Regenerate Alpine checksums

When an Alpine aport source file changes, regenerate its `sha512sums` with the
supported command instead of editing individual digests:

```sh
./fplinux checksum <aport>
```

The command updates only the canonical `APKBUILD` checksum block and refuses to
publish if its declared inputs change while it runs.

After the image and required source archives have been prepared, regeneration
can run without network access:

```sh
./fplinux checksum <aport> --offline
```

`./fplinux checksum` is the sole supported path for regenerating FPLinux aport
checksums. Do not run `abuild checksum` directly in the checkout or manually
replace individual digest lines.

## Build a target

```sh
./fplinux build <target>
./fplinux build <target> --jobs 8
```

`--jobs` limits parallel compilation. A matching selected bundle is reused;
otherwise the command rebuilds it from the current inputs. Target names are
discovered from `targets/`; use the [target index](../../targets/README.md) to
choose one.

After an online build has prepared the required inputs, an offline build miss
can run with networking disabled:

```sh
./fplinux build <target> --offline
```

If the required pinned environment is missing or stale, an offline build
asks for an online `./fplinux setup` first. A matching bundle remains usable
offline.

### Development profiles

A target may declare a development-only build profile at
`targets/<target>/profiles/<profile>/profile.toml`. Profiles are small deltas
over the target: they may enable or disable boolean kernel symbols, add Linux
patch/copy/append inputs, add or exclude rootfs packages, and select either the
`usb-ncm` or `none` host transport. Profile source paths are relative to that
profile directory.

The manifest has one exact, unversioned shape:

```toml
name = "example"

[linux]
config_enable = []
config_disable = []
patches = []
copies = []
appends = []

[rootfs]
packages = []
exclude_packages = []

[runtime]
transport = "none"
```

Build and check a declared profile explicitly:

```sh
./fplinux check kernel --profile <profile>
./fplinux build <target> --profile <profile>
```

The ordinary commands without `--profile` use only each target's default
context; declared profiles are checked only when named explicitly. Profiles
cannot be passed to `package`, `console` or `verify`, and they are never release
inputs.

`transport = "none"` changes only host-side runner behavior. The profile must
also exclude any in-image gadget or SSH services it does not want. See
[Loading from a source checkout](LOADING.md#from-a-source-checkout) for the matching
run command and its evidence boundary.

Default and named builds keep isolated current output and work state. Selecting
a profile never replaces the default target bundle.

After building, follow [Loading from a source checkout](LOADING.md) to configure the
runtime host, load the selected image, reconnect, and verify a running default
target.

## Logs, cache, and parallel commands

Build and check print compact stage status. Add `--verbose` to stream their tool
output; complete logs are retained under `.cache/logs/` and the command reports
their location on failure.

Public commands serialize writes to shared build state. Target output is kept
under `.cache/out/<target>/`; treat it as generated data, not as a user-managed
workspace.

Prepared Linux, Sparse, rootfs, staged workspaces, profile logs and locally
built APKs use bounded managed slots. Successful commands discard superseded
managed state, while `prune` handles interrupted or orphaned entries. Cache
records have no migrations or fallback readers: unknown or mismatched state is
a cache miss and is replaced only inside its managed slot.

Inspect cache cleanup candidates before deleting generated data:

```sh
./fplinux prune
./fplinux prune --json
./fplinux prune --apply
```

`prune` without `--apply` is read-only. Unknown, old, or mismatched generated
entries are cache misses and are not migrated.

## What a build proves

A successful build proves that the current checkout produced the selected
bundle. It does not prove that the image boots or that a hardware feature works
on a phone. Target documents record feature-level qualification and limitations.

See [Release archives](RELEASES.md) to create a physical-qualification
candidate. To use the result on a phone, continue with
[Loading from a source checkout](LOADING.md).
