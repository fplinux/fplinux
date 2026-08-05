# Shared console stack

`common/` contains post-kernel userspace and runtime code that does not depend on
a phone memory map, register layout or bootstrap protocol.

- `run.py` is the shared RAM-only bundle runner. It validates the generic
  `fplinux.runtime/v1` manifest, referenced hashes and RAM image header, then
  checks the host Python version and every host tool's shared-library closure
  with `ldd`. Only after that preflight does it load
  `runner/platform_adapter.py` and call its fixed `run(bundle, runtime)` entry
  point.
- `rootfs/overlay/init` enters the shared init implementation.
- `rootfs/overlay/usr/libexec/fplinux/init` mounts the early filesystems, runs an
  optional target diagnostics hook, starts the external `usb-getty` worker with
  BusyBox `start-stop-daemon` and launches the local terminal. The worker uses
  BusyBox `getty` to give `/dev/ttyGS0` to each USB shell as its controlling
  terminal and writes lifecycle messages to `/dev/kmsg`.
- `../buildroot-external/package/fplinux-console/` builds the shared musl terminal
  against Linux VT/fbcon, PTY and normalized evdev interfaces.
- `host/fplinux-usb-console.c` is the descriptor-driven host USB terminal used
  by phone runners with explicit USB IDs.

Phone framebuffer and keypad drivers, pre-Linux screen composition, DTS wiring
and target runtime values stay under `targets/<phone>/`. SoC-specific host
translation stays in the fixed adapter declared by
`platforms/<soc>/platform.toml`; targets do not copy the shared runner or add a
launcher. Platform-neutral freestanding primitives such as the shared
boot-screen renderer live under [`bootstrap/`](../bootstrap/README.md); they are
not part of common post-kernel userspace. See
[`docs/porting/CONSOLE.md`](../docs/porting/CONSOLE.md) for the interface a new
console target must provide.
