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

## Format source

Format only the files being edited:

```sh
./fplinux format scripts/fplinux_cli/example.py docs/example.md
```

The command accepts one or more normalized repository-relative file paths. It
does not recurse into directories or provide a whole-checkout mode. Tracked and
non-ignored untracked project sources are accepted.

Formatting uses the same pinned tools and classification as the quality gate:

- C and headers: `clang-format`;
- Python: `ruff format`;
- Markdown, JSON, JSONC, `commitlint.config.mjs` and the uppercase keypad app
  JSON manifest: Prettier;
- TOML: Taplo;
- POSIX and Bash scripts recognized by their shebang, plus the POSIX sourced
  configurations `alpine/abuild.conf` and
  `alpine/aports/fplinux-micropythonos-storage/micropythonos.conf`: `shfmt`.

The npm-owned `package-lock.json` is excluded from formatting.

Files without a project formatter, including Devicetree sources and bindings,
Kconfig, Makefiles, patches, APKBUILDs, Containerfiles and plain text, are
rejected instead of being passed to a guessed tool. The checkout is never
mounted writable in the container. All selected files are formatted in a
private projection. Only after every formatter succeeds and the checkout is
confirmed unchanged is each changed source file replaced atomically.

## Check source

Run the complete uncached source-quality gate before committing or submitting
source changes:

```sh
./fplinux check --no-cache
./fplinux check --no-cache --jobs 1
./fplinux check --list
./fplinux check docs spelling
```

With no scopes, `check` runs the complete gate. Selected cacheable scopes reuse
an exact successful result when their current inputs match; otherwise they run
again. `--no-cache` reruns selected cacheable scopes. An ordinary build or RAM
run without source changes does not need to repeat the gate.

The complete gate checks up to three independent default kernel contexts at once.
Use `--jobs 1` to force serial kernel analysis on a memory-constrained host.
Source-only checks and named profiles do not gain extra work from a larger
limit. `--verbose` uses serial analysis so tool output can remain live.

The `docs` scope also rejects repository-local Markdown links whose file or
heading anchor does not exist.

Kernel, bootstrap, host and phone-userspace messages follow the shared
[logging contract](../reference/LOGGING.md). Project-owned source and tests
follow the [code style](../reference/CODE_STYLE.md).

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

Each build container starts with a 2 GiB memory budget.
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

### Two global profiles

FPLinux has two global profiles, declared once under `profiles/`:

- `default`: the system root is in RAM. Omitting `--profile` and explicitly
  selecting `--profile default` use the same build and runtime identity.
  zram uses ZSTD compression.
- `microsd-uboot`: the USB loader starts U-Boot in RAM, then Linux uses the
  microSD card as its persistent ext4 root. zram uses LZO-RLE compression.

Both profiles use the shared platform configuration and the selected target's
board configuration. A profile selects boot and storage policy, not individual
peripheral features. Board initialization, panel geometry, keys and fitted
firmware declarations remain target-owned. Separate feature profiles are not
accepted.

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
require `--candidate`; candidate packaging is not physical qualification.

The microSD build produces a partitioned `FPLINUX.img.xz`, containing a SHA-256
FIT on FAT32 and an ext4 system root. Building never writes removable media.
Follow [microSD system root](MICROSD_ROOT.md) for the layout, persistence and
shutdown rules.

### Local fitted device data

Some targets declare fitted device-data groups that must come from the exact
physical phone. Bluetooth firmware is delivered through the root filesystem.
The fitted headphone gain profile is built into the kernel image. FPLinux does
not download or supply either group.

Each group is independently optional. When a complete group is absent, the
build keeps that feature's generic behavior: Bluetooth remains unavailable,
and headphone audio uses the generic volume levels. If any part of a group
is present, the complete group must pass its declared size and digest checks; a
partial, damaged, or mismatched group fails the build.

Normal `build` and `run` commands consume only already prepared local data.
They do not read the phone's NAND.

After building, follow [Loading from a source checkout](LOADING.md) to configure
the host, load the selected image, reconnect and verify the running context.

#### Prepare device data

Prepare every group declared by `<target>` from one complete physical NAND
backup. One acquisition prepares the Bluetooth group and the independent
headphone gain group. The fitted profile must come from the exact phone
selected by `<target>`; do not reuse data from another handset or model. No
manual extraction, renaming or patching is needed. Preparation requires this
source checkout, not a standalone archive.

The complete command syntax is:

```sh
./fplinux device-data prepare TARGET [--from-dump PATH] [--jobs N] [--offline]
```

Start with the phone powered off and USB disconnected, then run:

```sh
./fplinux device-data prepare <target>
```

The command first builds and starts the default RAM system. Wait
until its loader asks for the phone; only then hold its boot key and connect the
powered-off phone. This is the normal loader-first sequence in
[Loading from a source checkout](LOADING.md#connect-the-phone); see
the selected phone's instructions in the [target index](../../targets/README.md)
for the target-specific key.

For a backup already saved from this exact physical NAND, use:

```sh
./fplinux device-data prepare <target> --from-dump PATH
```

This form does not build a loader or connect to the phone. `--jobs N` limits
parallel work when the read-only loader is built, and `--offline` requests that
build without network access.

The backup must contain the selected phone's complete physical NAND, with each
page's main bytes followed by its OOB bytes. Page size and accepted layouts are
target-specific. The command rejects an unsupported target, another layout or
length, damaged required data, and ambiguous selected-block mappings. A saved
input remains unchanged at its original path.

The dump, extracted originals, prepared groups, and any image containing them
are private and non-redistributable unless you have the necessary rights. They
are local inputs and are not supplied by the source checkout or its pinned build
environment. The live reader is read-only: it does not mount, erase, restore,
or otherwise write the phone's NAND or NV storage.

When preparation finishes, it prints the exact next commands:

```sh
./fplinux build <target>
./fplinux run <target>
```

Build the selected profile, then end the preparation RAM session with the
selected target's shutdown procedure
and disconnect USB. For the prepared RAM load, start `run` with the phone
again powered off and disconnected; wait for its loader invitation before
holding its boot key and connecting it.

## Logs, cache, and parallel commands

Build, check and format print compact stage status. Add `--verbose` to build or
check to stream their tool output. Complete logs are retained under
`.cache/logs/`, and each command reports their location on failure.

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
