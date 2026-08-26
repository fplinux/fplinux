# Shared pre-Linux components

`bootstrap/` owns freestanding components reused before Linux starts. They have
no Linux userspace dependency and do not contain SoC-specific loader policy or
phone-specific addresses, register setup, panel sequences, keymaps or assets.

The selected [platform](../platforms/README.md) owns reusable SoC integration
and the shared loader flow. A [target](../targets/README.md) owns board data,
board-specific hardware initialization, display presentation and diagnostics.
Shared bootstrap code may expose bounded callbacks for those platform- and
target-owned operations, but must not take over either contract.

Use this directory only for behavior already shared by current targets.
Target-neutral host and runtime tools belong in [`common/`](../common/README.md),
while post-kernel userspace, APKs and services belong in
[`alpine/`](../alpine/README.md).

See the [porting overview](../docs/porting/README.md) for the complete ownership
model and the project [documentation index](../README.md#documentation) for
shared user workflows. Project-owned sources follow the
[bootstrap C rules](../docs/reference/C_STYLE.md#bootstrap-code).
