# Debugging FPLinux

This guide covers diagnostics available in source-built development images.
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
./fplinux logs list --status failed --json
```

Runs are ordered by their recorded start time, newest first. The listing shows
the run ID, command, target, profile, recorded status, start time and duration in
seconds. Filters can be combined and are available on all three `logs` commands.
Statuses are `running`, `success`, `failed` and `interrupted`; they describe the
recorded command result, not phone health. Profiles apply to build and check
runs. An empty listing succeeds.

`--json` returns an array with `id`, `command`, `target`, `profile`, `status`,
`started_at`, `finished_at`, `duration_seconds`, `pid` and `path`. Times use UTC;
duration is elapsed time for an unfinished run. Missing target, profile or
finish time is `null`. `path` is the repository-relative run directory.

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

## Kernel tracing

The shared UMS9117 kernel configuration includes debugfs, tracefs, kprobe
events and the `irqsoff` tracer. No tracer or dynamic probe is active after
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
