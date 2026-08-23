# Shared host and runtime stack

`common/` owns target-neutral host and runtime components: the RAM-only bundle
runner, the USB keyboard client, and their shared runtime contracts. They must
not depend on a phone memory map, board register layout, or bootstrap protocol.

Phone-owned drivers, display and keypad setup, DTS wiring, and runtime values
belong under `targets/<phone>/`. Reusable SoC-specific host translation belongs
to the selected platform. Post-kernel userspace belongs in
[Alpine packages](../alpine/README.md); pre-Linux shared primitives belong in
[bootstrap](../bootstrap/README.md).

A target uses the shared runner through its manifest rather than copying a
runner or adding a target-specific launcher.
