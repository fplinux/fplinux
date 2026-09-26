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

Declared Linux patches are also accepted. Their affected C/H regions are
formatted with the pinned Linux `.clang-format` and LLVM's `clang-format-diff`
tool. The command reconstructs the source context, regenerates each selected
patch and checks that the remaining integration steps still apply. It retains
non-C contents without claiming to format Kconfig, Makefile or Devicetree syntax.
Patch inputs require the pinned Linux source archive; it is downloaded when
missing. Other patch series are not accepted by this formatter.

Files without a project formatter, including standalone Devicetree sources and
bindings, Kconfig, Makefiles, APKBUILDs, Containerfiles and plain text, are
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

The kernel check analyzes one context per target that offers the selected
profile, by default up to three at once. Use `--jobs 1` to force serial kernel
analysis on a memory-constrained host. A `--jobs` value above 1 requires the
`kernel` scope when scopes are named and cannot be combined with `--verbose`,
which uses serial analysis so tool output can remain live.

The `docs` scope also rejects repository-local Markdown links whose file or
heading anchor does not exist, and documentation site pages that are neither in
the `nav` of `mkdocs.yml` nor matched by its `not_in_nav`.

The `kernel` scope checks formatting inside Linux patches as well as standalone
C/H sources. Kconfig and Kbuild fragments are checked as changes to their Linux
destination files, before the complete configuration and compilation checks.
Changed Devicetree bindings use the kernel's `yamllint` configuration and
`dt_binding_check`; built board trees use `dtbs_check`.

Kernel, bootstrap, host and phone-userspace messages follow the shared
[logging contract](../reference/LOGGING.md). Project-owned source and tests
follow the [code style](../reference/CODE_STYLE.md).

## Run selected tests

Use `test` to run unittest tests in the same pinned Kern image as `check python`:

```sh
./fplinux test tests.small.test_common
./fplinux test tests.small.test_common.FileDigestTests
./fplinux test tests.small.test_common.FileDigestTests.test_empty_short_and_multibuffer_files_match_sha256_vectors
./fplinux test --tier host_tool
```

Supply one or more dotted test names, or select one tier with `--tier`.
The tiers are `small`, `host_process`, `host_tool`, `artifact` and
`public_workflow`. With neither selector, all five tiers run in that order.
`--verbose` shows individual test names and streams output; `--failfast` stops
on the first failure or error. Complete output is saved with the command's logs.

The command uses a read-only snapshot of project sources, without network access
inside the test container. It prepares the pinned environment when needed, just
as `check` does. Tests retain their tier time limits; a selection spanning tiers
has their combined time budget.

Exit status is 0 for success, 1 for test failures or unresolved names, 2 for
invalid command arguments, and 5 when unittest finds no tests. Ctrl+C returns 130. A selected test run always executes and never creates or refreshes a
successful `check` receipt. It does not run linters or replace `check python`
or the complete quality gate.

## Preview the documentation site

MkDocs builds the documentation site from a copy of the documentation pages
kept at their repository paths. Install the pinned site tools once, then
collect the pages and start the local preview from the repository root:

```sh
python3 -m venv .cache/site/venv
.cache/site/venv/bin/pip install --require-hashes -r site/requirements.txt
python3 scripts/site_collect.py
.cache/site/venv/bin/mkdocs serve
```

`site_collect.py` replaces `.cache/site/src` with the current pages; run it
again after editing a page. `mkdocs serve` prints the local address and reloads
when the collected pages or `mkdocs.yml` change. A page missing from the site
navigation, a broken link or a missing anchor stops the build.

## Build an ARM diagnostic program

For a single C source file in this checkout, build a static ARMv7 hard-float
musl executable for the phone:

```sh
./fplinux probe-build .cache/tools/diag.c --output .cache/tools/diag-arm
```

Create the source file first. Both paths must be repository-relative and free
of symlinks. The source must be a regular file, and the output must be inside
`.cache/tools`. The command
uses the pinned Kern image, project headers and locked Alpine packages. It
prepares the ARM sysroot when needed, downloading missing locked inputs on the
first run. If the build image is not prepared, run `./fplinux setup` first.

`probe-build` only compiles the program. It does not add it to a phone image,
upload it or run it on a phone. A failed compile leaves any existing output
unchanged.

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

## Create a new target

For a UMS9117 phone that has no target yet, create a headless target:

```sh
./fplinux target new TARGET --brand BRAND --product PRODUCT [--compatible VENDOR,DEVICE]
```

`TARGET` names the new `targets/TARGET` directory and is the target name for
every other command. It consists of lowercase letters and digits separated by
single hyphens. `--brand` and `--product` are the public names defined by the
[identity contract](../reference/IDENTITY.md); the hardware-code list starts
empty. Without `--compatible`, the board compatible is formed from the lowercase
brand and product, for example `hammer,horizon-lte`.

The target uses the only platform, `ums9117`, by default. `--platform NAME`
selects the platform in `platforms/NAME` and is required only when more than
one platform exists.

The new target loads
[headless](../../platforms/ums9117/README.md#target-requirements): the phone
shows no boot screen, and the loader applies no board pin settings. Linux
provides the USB session and read-only internal NAND access; it has no display,
keypad, audio, Bluetooth or microSD support, and only the default profile is
available. In the `[adapter]` table of its `target.toml`, `boot_instructions`
names `*` as the boot key and `exec_distance` is `0`. Its `README.md` records
the FPLinux support of every feature and application as **Unknown**.

The command refuses an existing target directory and leaves nothing behind
when a name or identity field is invalid. It lists the created files and ends
with the next step:

```sh
./fplinux build TARGET && ./fplinux run TARGET
```

The command does not add the new target to the
[target index](../../targets/README.md). To boot the new target and back up
its NAND, follow [Bring up a new UMS9117 phone](../porting/NEW_PHONE.md).

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

The current targets declare fitted device-data groups that must come from the
exact physical phone. Bluetooth firmware and FM settings are delivered through
the root filesystem. The fitted audio profile is built into the kernel image;
it holds the headphone, speaker and combined-output gains, the stock equalizer
and ALC processing for each of those outputs and, on phones that vibrate
through their speaker, the vibrate tone. FPLinux does not download or
supply these phone-specific inputs.

Each group is independently optional. When a complete group is absent, the
build keeps that feature's generic behavior: Bluetooth and FM remain
unavailable, headphone audio uses the generic volume levels, and no speaker
output is created. If any part of a group is present, the complete group must
pass its declared size and digest checks; a partial, damaged or mismatched
group fails the build.

Normal `build` and `run` commands consume only already prepared local data.
They do not read the phone's NAND.

A target may also declare the `board-maps` group, which its platform extracts
from the phone's stock firmware in the same NAND backup. The group holds the
loader pin map `pinmap.bin` and keymap `keymap.bin`. Their declarations omit the
size, because each phone's maps have their own. The loader does not read them
and takes its maps only from the target's asset lock. Preparing the group
also writes the board report `reports/board-maps/board-report.json` into the new
generation and prints its path. The report lists the board values found in the
stock firmware, where each came from, the values that were not found and the
decisions left to a person; builds never read it. The extraction runs a host
tool from the target's current build, so build the target before preparing its
device data from a saved backup. Each platform describes what its report
contains; see
[UMS9117 board data](../../platforms/ums9117/README.md#board-data-from-the-stock-firmware).

After building, follow [Loading from a source checkout](LOADING.md) to configure
the host, load the selected image, reconnect and verify the running context.

#### Prepare device data

Prepare every group declared by `<target>` from one complete physical NAND
backup. This prepares Bluetooth, FM, the independent audio gain group and, when
declared, the board maps.
The fitted data must come from the exact phone selected by `<target>`; do not
reuse data from another handset or model. No manual extraction, renaming or
patching is needed. Preparation requires this source checkout, not a standalone
archive.

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
page's main bytes followed by its OOB bytes. Its page layout comes from the
geometry receipt `PATH.json` that [`nand backup`](#save-a-nand-backup) writes
beside it. The receipt must describe exactly the bytes at `PATH`. A target that
declares its NAND chip also accepts a backup without a receipt; when both are
present, they must report the same chip and page size. Without either, the
command refuses the backup instead of inferring a layout from its length.
Preparation interprets only 2048 main bytes per page, 64 pages per block and
65536 pages in total. The command also rejects an unsupported target, a length
that does not match the layout, damaged required data, and ambiguous
selected-block mappings. A saved input and its receipt remain unchanged at their
original paths.

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

#### Save a NAND backup

For a phone already running the selected build, save its complete physical-page
stream with:

```sh
./fplinux nand backup <target> PATH [--profile NAME]
```

The command first opens the phone's raw NAND reader, which identifies the
fitted chip, and reads the geometry the reader reports. It stops before reading
any NAND page when the running kernel does not identify the chip, or when the
chip or its page size differs from the one the target declares. The expected
image size comes from the reported geometry, not from the target.

The command checks that the complete stream has that size, computes a SHA-256
digest and atomically publishes the file with mode `0600`. It then atomically
writes the geometry receipt `PATH.json` beside it, also with mode `0600`. The
receipt is a JSON object with these fields:

| Field                            | Value                                                 |
| -------------------------------- | ----------------------------------------------------- |
| `id_bytes`                       | chip ID bytes as hexadecimal, manufacturer byte first |
| `chip`                           | chip name reported by the reader                      |
| `page_main_bytes`, `oob_bytes`   | main and OOB bytes per page                           |
| `pages_per_block`, `block_count` | pages per erase block and number of blocks            |
| `raw_bytes`                      | size of the saved image                               |
| `sha256`                         | SHA-256 digest of the saved image                     |
| `target`                         | target used for the backup                            |

An incomplete transfer leaves an existing destination and its receipt
unchanged. The digest identifies the saved bytes; the command does not compare
them with a second device read. Restoring NAND is unsupported. Treat the backup
as private device data. The receipt holds only the fields above and no NAND
contents; it can be shared, for example attached to an issue report.

To see what the reader reports without saving a backup, run:

```sh
./fplinux nand identify <target> [--profile NAME]
```

It opens the reader the same way and prints the reader's report unchanged, one
`key=value` line each for `id_bytes`, `chip`, `page_main_bytes`, `oob_bytes`,
`pages_per_block`, `block_count`, `raw_bytes`, `geometry_source`, `feature_a0`,
`feature_b0` and `feature_c0`. For a chip that the running kernel does not
identify, the report shows `chip=unknown`, zero geometry values and
`geometry_source=unknown`, together with the chip's ID bytes and feature
register values.

## Inspect built artifacts

```sh
./fplinux inspect bundle nokia-ta1618
./fplinux inspect bundle nokia-ta1618 --profile microsd-uboot
./fplinux inspect archive path/to/FPLinux.zip
./fplinux inspect apk path/to/package.apk
```

`bundle` reads the published current generation for the selected target and
profile, prints its identity and file sizes and SHA-256 hashes, and checks the
files against their build manifest. It does not rebuild or check whether the
source checkout has changed since that build. APKs and debug files are included
in the file listing.

`archive` reads a FPLinux candidate or release ZIP, reports its recorded identity
and file listing, and checks every payload against the enclosed `SHA256SUMS`.
Missing, extra or mismatched files cause failure. The checks establish internal
byte consistency, not authenticity or phone support.

`apk` displays `.PKGINFO` and the file list of an APK v2 package, including link
destinations. It does not install the package, run its scripts or verify its
signature. Neither archive command extracts files.

Inspection needs no Kern environment or phone connection. Bundle inspection
holds the shared cache lock while reading the selected generation; inspecting
a ZIP or APK does not take that lock or create cache state. Output is text;
exit status is 0 on success, 1 on an inspection or checksum error, 2 for invalid
arguments and 130 after Ctrl+C.

## Logs, cache, and parallel commands

Build, check, test and format print compact stage status. Add `--verbose` to
build, check or test to stream their tool output. Complete logs are retained under
`.cache/logs/`, and each command reports their location on failure.
Use [`logs list`, `logs show` and `logs follow`](DEBUGGING.md#build-and-command-logs)
to find and read command output without looking up stage filenames.

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
./fplinux prune --apply
```

`prune` without `--apply` is read-only. Unknown, old, or mismatched generated
entries are cache misses and are not migrated.

## What a build proves

A successful build proves that the current checkout produced the selected
bundle. It does not prove that the image boots or that a hardware feature works
on a phone. Target documents record current feature support and limitations.

See [Release archives](RELEASES.md) to create a phone-test candidate. To use the
result on a phone, continue with
[Loading from a source checkout](LOADING.md).
