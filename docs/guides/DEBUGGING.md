# Debugging FPLinux

This guide covers diagnostics available in source-built images. Select
`--build-type debug` for kernel tracing, loadable diagnostic modules, DMATEST
and `/dev/mem`; the default `release` build omits those facilities. See
[build types](BUILDING.md#build-types) for the complete selection rules.
Debug output and tracing help investigate a running RAM session; they do not
prove hardware support or make a payload release-ready.

## Build and command logs

Commands such as `build`, `check` and `test` retain complete stage logs under
the reported `.cache/logs/` path. Add `--verbose` when live tool output is useful.
Kernel, bootstrap, host and phone-userspace messages follow the shared
[logging contract](../reference/LOGGING.md).

### Find a run

```sh
./fplinux logs list
./fplinux logs list --command build --target nokia-ta1618 --profile default
./fplinux logs list --status failed
```

Runs are ordered by their recorded start time, newest first. The listing shows
the run ID, command, target, profile, recorded status, start time and duration in
seconds. Filters can be combined and are available on all three `logs` commands.
Statuses are `running`, `success`, `failed` and `interrupted`; they describe the
recorded command result, not phone health. Profiles apply to build and check
runs. An empty listing succeeds.

Times use UTC; duration is elapsed time for an unfinished run. Missing target
or profile is shown as `-`.

### Read stage output

```sh
./fplinux logs show latest --command check --failed
./fplinux logs show latest --command test --stage selected --tail 20
./fplinux logs follow latest --command build
```

Omit the run argument to select `latest`, or copy a run ID from `logs list`.
A directory basename also works when it identifies only one matching run.
`latest` selects after filtering; `follow` stays attached to that invocation
even if another command starts.

Output includes nested container stages. `--stage` selects a stage name or
the full stage path printed in a heading. A name shared by several stages
selects all of them. `show --failed` restricts output to failed or interrupted
stages of the selected run; it does not select a different run.

The initial output is the last 40 lines of each selected stage. Use `--tail N`
to change that limit, or `--tail 0` to omit existing output. `follow` then prints
appended output and newly created stages until the run records completion.
Output is grouped by stage; it is not a globally ordered merge, and a verbose
parent log can repeat child output. Ctrl+C stops only the reader.

Viewing logs does not need Kern, take the build lock, create a new journal or
change receipts. Only recognized command journals are listed. If a running
record has no surviving process, `follow` reports the missing final status
instead of waiting indefinitely. A recorded status alone is not proof that a
process is still alive.

Exit status is 0 after successful inspection, including inspection of a failed
run; 1 for a missing or ambiguous run, missing stage or read failure; 2 for
invalid arguments; and 130 after Ctrl+C.

## Inspect built artifacts

```sh
./fplinux inspect bundle nokia-ta1618
./fplinux inspect bundle nokia-ta1618 --profile microsd-uboot
./fplinux inspect archive path/to/FPLinux.zip
./fplinux inspect apk path/to/package.apk
```

`bundle` reads the published current generation for the selected target and
profile and build type, prints its identity and file sizes and SHA-256 hashes, and checks the
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
a ZIP or APK does not take that lock or create cache state. The commands above produce text;
exit status is 0 on success, 1 on an inspection or checksum error, 2 for invalid
arguments and 130 after Ctrl+C.

### Measure image footprint

Measure the current verified bundle without building or connecting a phone:

```sh
./fplinux inspect footprint nokia-ta1618 --profile default --build-type release
./fplinux inspect footprint nokia-ta1618 --json > before.json
./fplinux inspect footprint nokia-ta1618 --json > after.json
./fplinux inspect footprint-diff before.json after.json
./fplinux inspect footprint-diff before.json after.json --json
```

Save `before.json` before rebuilding and `after.json` after rebuilding the
context being compared. `--json` produces a machine-readable report. The
measurement separates boot artifacts, kernel zImage, compressed embedded
initramfs, compressed RAM backing, unpacked root filesystem, optional APK
archives and host debug files.
These layers overlap; adding them does not produce a meaningful total.

Package sizes count their owned regular-file and symlink payload, excluding
dependencies. They are not installed disk usage or runtime RAM consumption.
`footprint-diff` compares two saved reports and lists artifact byte deltas and
added, removed or changed packages, files and optional APKs. It also reports
whether root filesystem content is identical. Compression savings alone do not
establish a userspace or memory reduction.

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

## Kernel tracing

The UMS9117 `debug` kernel includes debugfs, tracefs, kprobe events and the
`irqsoff` tracer. No tracer or dynamic probe is active after
boot. Inspect the current state from the phone shell as root:

```sh
cat /sys/kernel/tracing/available_tracers
cat /sys/kernel/tracing/current_tracer
cat /sys/kernel/tracing/kprobe_events
```

Tracing and dynamic probes can destabilize the kernel and consume the phone's
limited RAM. Disable probes in any additional trace instances first. Then
remove root probes, restore the `nop` tracer and release the root trace buffer:

```sh
echo 0 > /sys/kernel/tracing/events/kprobes/enable
echo > /sys/kernel/tracing/kprobe_events
echo nop > /sys/kernel/tracing/current_tracer
echo 1 > /sys/kernel/tracing/free_buffer
```

The selected phone's [target document](../../targets/README.md) states its
hardware support boundary. Use [Building FPLinux](BUILDING.md) for the
source workflow and [Loading from a source checkout](LOADING.md) for the physical
session.
