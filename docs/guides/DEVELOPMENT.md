# Developing FPLinux

Use these source-checkout workflows when editing and submitting changes.
Prepare the host with [Building FPLinux](BUILDING.md#requirements-and-setup);
project-owned sources and tests follow the [code style](../reference/CODE_STYLE.md).

## Format source

Format only the files being edited:

```sh
./fplinux format scripts/fplinux_cli/example.py docs/example.md
```

The command accepts one or more normalized repository-relative file paths. It
does not recurse into directories or provide a whole-checkout mode. Tracked and
non-ignored untracked project sources are accepted.

Formatting and checking calculate the same canonical bytes with the pinned tools:

- C and headers: `clang-format`;
- Python: Ruff's import-order fix and formatter;
- Markdown, JSON, JSONC, YAML and maintained JavaScript configurations: Prettier;
- TOML: field ordering followed by Taplo;
- Devicetree sources: bounded property ordering with values, includes and node
  order preserved;
- APKBUILD, Kconfig, Makefile, INI, EditorConfig and assembly: safe whitespace
  normalization with declarations and execution order preserved;
- POSIX and Bash scripts recognized by their shebang, plus the POSIX sourced
  configuration `alpine/abuild.conf`: `shfmt`.

Supported `.in` templates use their rendered format's rules. Field and section
order, sequence preservation and whitespace limits are defined by the
[source-format contract](../reference/style/FORMATS.md).
The npm-owned `package-lock.json` is excluded from formatting.

Declared Linux patches are also accepted. Their affected C/H regions are
formatted with the pinned Linux `.clang-format` and LLVM's `clang-format-diff`
tool. The command reconstructs the source context, regenerates each selected
patch and checks that the remaining integration steps still apply. It retains
non-C contents without claiming to format Kconfig, Makefile or Devicetree syntax.
Patch inputs require the pinned Linux source archive; it is downloaded when
missing. Other patch series are not accepted by this formatter.

Files without a project formatter, including Containerfiles and ordinary plain
text, are rejected. The checkout is never mounted writable in the container.
All selected files are formatted in a
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

Use `test` to run pytest workloads with the same pinned dependencies as
`check python`:

```sh
./fplinux test tests.small.workspace.test_snapshot
./fplinux test tests.small.dependencies.test_dependency_inputs
./fplinux test tests.host_process.quality.test_test_runner
./fplinux test 'tests/small/workspace/test_snapshot.py::WorkspaceSnapshotTests::test_snapshot_recipe_includes_file_mode'
./fplinux test --tier host_tool
```

Supply one or more dotted module, class or method names, pytest node selectors
under `tests/<tier>/`, or select one tier with `--tier`. Node selectors use
`path.py::Class::method` or `path.py::test_function`; append `[case-id]` to select
one parameterized case. Quote selectors containing brackets in the shell.
The tiers are `small`, `host_process`, `host_tool`, `artifact` and
`public_workflow`. With neither selector, all five tiers run in that order.
`--verbose` shows individual test names and streams output; `--failfast` stops
on the first failure or error. Complete output is saved with the command's logs.

The command runs against a captured snapshot of project sources. Most tests run
in the pinned Kern image with the sources mounted read-only and network disabled.
Source-cache ownership and image-publication metadata tests run on the host,
where real subordinate UID/GID mappings are available. Their temporary Python
environment uses the same locked dependencies without installing system packages.
They require the host namespace support used by `setup`; unavailable mappings
fail the command instead of skipping those tests.

The command prepares the pinned environment when needed, just as `check` does.
Host and container execution share their tier time limit; a selection spanning
tiers has their combined time budget. Explicit selectors retain their order and
repetitions across both environments.

Exit status is 0 for success, 1 for test failures or unresolved names, 2 for
invalid command arguments, and 5 when pytest finds no tests. Ctrl+C returns 130.
A selected test run always executes and never creates or refreshes a
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

After [restoring a dependency snapshot](DEPENDENCIES.md), install the saved
site wheels without using a package index:

```sh
.cache/site/venv/bin/pip install --no-index --find-links .cache/downloads/site --require-hashes -r site/requirements.txt
```

`site_collect.py` replaces `.cache/site/src` with the current pages; run it
again after editing a page. `mkdocs serve` prints the local address and reloads
when the collected pages or `mkdocs.yml` change. A page missing from the site
navigation, a broken link or a missing anchor stops the build.

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

## Logs, cache, and parallel commands

Build, check, test and format print compact stage status. Add `--verbose` to
build, check or test to stream their tool output. Complete logs are retained under
`.cache/logs/`, and each command reports their location on failure.
Use the [command log viewer](DEBUGGING.md#build-and-command-logs) to find and
read complete output.

Public commands serialize writes to shared build state. Target output is kept
under `.cache/out/<target>/`; treat it as generated data, not as a user-managed
workspace.

## Clean generated state

Kernel compilation uses a shared local ccache under `.cache/ccache/`. Its
automatic cleanup limits the cache to 1 GiB. It can reuse compilations across
targets and profiles when their inputs match; the first fill can take longer
than an uncached compilation. It does not change the selected kernel features
or the phone's package set.

Host tools are reused from `.cache/host-tools/` when their source inputs and
pinned build environment match. A driver change does not rebuild unchanged host
tools. Cached tools still pass their binary checks and declared self-tests.

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
