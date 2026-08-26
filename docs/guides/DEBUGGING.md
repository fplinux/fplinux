# Debugging FPLinux

This guide covers diagnostics available in source-built development images.
Debug output and tracing help investigate a running RAM session; they do not
prove hardware support or qualify a release payload.

## Build and command logs

`./fplinux build` and `./fplinux check` retain complete stage logs under the
reported `.cache/logs/` path. Add `--verbose` when live tool output is useful.
Kernel, bootstrap, host and phone-userspace messages follow the shared
[logging contract](../reference/LOGGING.md).

## Nokia kernel tracing

The default Nokia 3210 4G (TA-1618) kernel includes debugfs, tracefs, kprobe
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

The [Nokia target document](../../targets/nokia-ta1618/README.md) states the
phone-specific support boundary. Use [Building FPLinux](BUILDING.md) for the
source workflow and [Loading from a source checkout](LOADING.md) for the physical
session.
