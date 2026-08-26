# Console port contract

FPLinux's local terminal is shared post-kernel userspace packaged through the
[Alpine layer](../../alpine/README.md). A phone port supplies standard Linux
display and input interfaces; the terminal does not contain panel-register or
scan-code knowledge.

## Required interfaces

The local terminal needs:

- a usable text virtual terminal backed by `fbcon`;
- `/dev/ptmx` and `devpts` for the interactive shell;
- at least one evdev keypad with the normalized keys below.

The terminal discovers compatible evdev devices by capability, not event number
or phone identity. Additional compatible input devices are optional. A target
with the host-keyboard bridge also provides its designated generic-serial input
path and a persistent uinput keyboard device.

## Normalized keypad interface

The keypad driver reports normal Linux input codes. The console requires digits
`KEY_0` through `KEY_9`, `KEY_TAB`, `KEY_BACKSPACE`, `KEY_ENTER`,
`KEY_KPASTERISK`, `KEY_KPDOT`, and the four arrow keys. Target keymaps choose
which physical keys provide them.

The terminal uses `TERM=linux`. It keeps shell output and console input on the
primary virtual terminal, so a port must not substitute an escape-sequence
overlay for `fbcon`. The user-visible keypad behavior belongs to the
[local console feature](../features/LOCAL_CONSOLE.md), not to each target port.

## Target responsibilities

A console target owns:

- its framebuffer or DRM driver and the mode exposed through `fbcon`;
- keypad scan, wiring and conversion to the normalized Linux input codes;
- DTS and built-in kernel configuration required for evdev, framebuffer console,
  virtual terminals and the shell's PTY support;
- when used, the host-keyboard bridge's uinput and generic-serial dependencies.

The target's support document states which of these interfaces has been
exercised on its physical hardware. See the [target template](TARGET.md) and
the selected platform document for board-specific requirements.

See the [porting overview](README.md) for the complete layer boundary and the
project [documentation index](../../README.md#documentation) for shared guides,
features and applications.
