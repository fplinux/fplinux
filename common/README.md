# Shared host and runtime tools

`common/` owns target-neutral host and runtime components: the RAM-only bundle
runner, the USB keyboard client, and their shared runtime contracts. They must
not depend on a phone memory map, board register layout, or bootstrap protocol.

Phone-owned drivers, display and keypad setup, DTS wiring, and runtime values
belong under `targets/<phone>/`. Reusable SoC-specific host translation belongs
to the selected platform. Phone userspace, APKs and services belong in
[Alpine packages](../alpine/README.md); pre-Linux shared primitives belong in
[bootstrap](../bootstrap/README.md).

Components installed in the phone root filesystem or managed as services remain
Alpine-owned even when they are target-neutral. SoC integration remains
[platform-owned](../platforms/README.md), and phone-specific runtime values
remain [target-owned](../targets/README.md).

A target uses the shared runner through its manifest rather than copying a
runner or adding a target-specific launcher.

See the [porting overview](../docs/porting/README.md) for the complete ownership
model and the project [documentation index](../README.md#documentation) for
supported user workflows. Project-owned host C follows the
[host-tool rules](../docs/reference/C_STYLE.md#host-tools).
