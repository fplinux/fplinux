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

Records such as `*_LINUX_BOOTSTRAP event=stage stage=... message=...` are diagnostics, not
the handoff control channel. Do not parse them to authorize a transition. The
session-bound binary exchange owns that decision. Human-facing boot-screen text
is a separate interface.

Records identify their event before numeric fields; free-form `message` text
comes last. Errors use an `error` field rather than a reserved stage number.
Keep these diagnostics separate from the binary handoff protocol.

## Host CLI

Public commands report progress through the shared stage reporter. New command
paths add meaningful stages instead of printing an independent progress format.
Keep documented machine-readable output stable.

Diagnostics and progress go to standard error; results and machine-readable
output go to standard output. Keep child-command streams and loader prompts in
their established channels. Repository diagnostics use the shared error formatter
and the `fplinux:` prefix; `common.fail()` ends an operation on a single error.
Standalone runner and SSH errors use `fplinux run:` and
`fplinux ssh:` without depending on the repository package.

Report expected file and process failures with their cause, without a Python
traceback. Ctrl+C terminates with shell status 130. Unexpected exceptions retain
their traceback; a stage records it in its log. Do not hide a later error merely
because an earlier one was reported. Nested stages restore the caller's reporting
context, and a failed run cannot become successful when logs are closed.

Describe the operation actually completed. A saved file and its computed digest
are not a byte-for-byte comparison with an independent source. Use `verified`
only when the named comparison has actually succeeded.

## Phone userspace

Messages written to `/dev/kmsg` use a component prefix and a severity matching
the outcome; failures are not informational messages. Long-running services log
state transitions instead of repeating the same unavailable-device error on
every retry. The local VT may use a compact visual format when it is not parsed
as a protocol.

Supervised services use OpenRC's `output_logger` and `error_logger` hooks with
the shared kernel-log forwarder. It writes bounded records and splits long
lines into fragments. Do not use `/dev/kmsg` as an ordinary stream log file.
Choose stream priorities from the producer's meaning: stderr from an external
daemon can contain ordinary diagnostics, not only errors.

## Embedded projects

Use U-Boot's `log_*` interface with an explicit category. Keep boot failures
visible and routine controller-release messages at debug level.

Use [Hardware debugging](../guides/DEBUGGING.md) for diagnostic logs and
tracing. The [code style](CODE_STYLE.md) covers implementation and identifiers,
while the [porting overview](../porting/README.md) defines which layer owns a
new component.
