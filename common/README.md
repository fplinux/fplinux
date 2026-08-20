# Shared host and runtime stack

`common/` contains code shared across phone targets without depending on a phone
memory map, register layout or bootstrap protocol.

- `run.py` is the shared RAM-only bundle runner. It validates the generic
  `fplinux.runtime/v1` manifest, referenced hashes and RAM image header, checks
  the host Python version, then loads the fixed
  `runner/platform_adapter.py` entry point. Bundled native host tools are built
  as static Linux/x86-64 executables and need no host libusb/libudev/libc
  closure.
- `host/fplinux-usb-console.c` is the descriptor-driven host USB terminal used
  by phone runners. It uses interface 0 for shell and transfer modes, and
  interface 1 for `--keyboard`.

Post-kernel phone userspace belongs to conventional APK packages under
[`../alpine/aports/`](../alpine/README.md). OpenRC owns service supervision;
there is no common rootfs overlay or FPLinux-specific init process.

Phone framebuffer and keypad drivers, pre-Linux screen composition, DTS wiring
and target runtime values stay under `targets/<phone>/`. SoC-specific host
translation stays in the fixed adapter declared by
`platforms/<soc>/platform.toml`; targets do not copy the shared runner or add a
launcher. Platform-neutral freestanding primitives such as the shared
boot-screen renderer live under [`bootstrap/`](../bootstrap/README.md). See
[`docs/porting/CONSOLE.md`](../docs/porting/CONSOLE.md) for the interface a
console target must provide.
