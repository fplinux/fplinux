# Logging contract

FPLinux keeps kernel, bootstrap, host and phone-userspace messages as separate
interfaces. Do not force one textual format across these layers.

## Kernel

Use `dev_*` for device drivers and `pr_*` only when no device context exists.
Use `dev_err_probe()` for probe failures that may defer. A healthy driver should
normally stay quiet: rate-limit recurring faults and put raw register snapshots
in `dev_dbg`, debugfs or tracepoints. Reserve `dev_emerg` and `pr_emerg` for a
system-wide unusable state, not a local peripheral failure.

Use `%pe` with `ERR_PTR(error)` for a negative errno outside probe. Counts,
register values and statuses that can represent success remain numeric. Keep
messages concise and avoid repeating the device or chip name supplied by the
logging helper. Preserve errors that explain lost functionality, such as an
audio underrun or a failed shutdown, at a visible severity.

## Bootstrap

Records such as `*_LINUX_BOOTSTRAP stage=... message=...` are diagnostics, not
the handoff control channel. Do not parse them to authorize a transition. The
session-bound binary exchange owns that decision. Human-facing boot-screen text
is a separate interface.

## Host CLI

Public commands report progress through the shared stage reporter. New command
paths add meaningful stages instead of printing an independent progress format.
Keep documented machine-readable output stable.

Standalone runner and SSH errors use `fplinux run:` and `fplinux ssh:`
without depending on the repository package. Keep loader invitations and
transfer results on standard output; retries and SSH connection diagnostics
belong on standard error. An unreadable bundle file reports its I/O error
without a Python traceback.

Report expected file and process failures with their cause, without a Python
traceback. Ctrl+C terminates with shell status 130. Unexpected exceptions retain
their traceback; a stage records it in its log. Do not hide a later error merely
because an earlier one was reported. Nested stages restore the caller's reporting
context, and a failed run cannot become successful when logs are closed.

## Phone userspace

Messages written to `/dev/kmsg` use a component prefix and a severity matching
the outcome; failures are not informational messages. Long-running services log
state transitions instead of repeating the same unavailable-device error on
every retry. The local VT may use a compact visual format when it is not parsed
as a protocol.

Use [Hardware debugging](../guides/DEBUGGING.md) for diagnostic logs and
tracing. The [code style](CODE_STYLE.md) covers implementation and identifiers,
while the [porting overview](../porting/README.md) defines which layer owns a
new component.
