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
  optional target diagnostics hook, starts the external `usb-getty` and
  `fplinux-input` workers with BusyBox `start-stop-daemon`, then launches the
  local terminal. `usb-getty` gives `/dev/ttyGS0` to each USB shell as its
  controlling terminal. `fplinux-input` turns event lines from `/dev/ttyGS1`
  into a persistent uinput keyboard.
- `../buildroot-external/package/fplinux-console/` builds the shared musl terminal
  against Linux VT/fbcon, PTY and normalized evdev interfaces. It accepts one
  primary and up to three additional matching input devices.
- `../buildroot-external/package/fplinux-input/` builds the gadget-serial to
  uinput bridge.
- `host/fplinux-usb-console.c` is the descriptor-driven host USB terminal used
  by phone runners. It uses interface 0 for shell and transfer modes, and
  interface 1 for `--keyboard`.

Phone framebuffer and keypad drivers, pre-Linux screen composition, DTS wiring
and target runtime values stay under `targets/<phone>/`. SoC-specific host
translation stays in the fixed adapter declared by
`platforms/<soc>/platform.toml`; targets do not copy the shared runner or add a
launcher. Platform-neutral freestanding primitives such as the shared
boot-screen renderer live under [`bootstrap/`](../bootstrap/README.md); they are
not part of common post-kernel userspace. See
[`docs/porting/CONSOLE.md`](../docs/porting/CONSOLE.md) for the interface a new
console target must provide.
