# Console port contract

FPLinux's local terminal is shared post-kernel userspace packaged through the
[Alpine layer](../../alpine/README.md). A phone port supplies standard Linux
display and input interfaces; the terminal does not contain panel-register or
scan-code knowledge.

## Required interfaces

The local terminal needs:

- a usable text virtual terminal backed by `fbcon`;
- `/dev/ptmx` and `devpts` for the interactive shell;
- the phone keypad described below.

A target with the host-keyboard bridge also provides its designated
generic-serial input path and a persistent uinput keyboard device.

## Phone keypad interface

The keypad driver publishes the phone keypad and its phone key codes as
defined in the [input contract](../reference/INPUT.md). The terminal finds the
keypad by its evdev `phys` string rather than its event number, and opens it
again after it disappears. It uses the digits, `*`, `#`, the D-pad, the centre
key, both soft keys and the dial key; it ignores the power key. The target
keymap chooses which physical keys provide these codes.

The Linux VT keyboard handler does not bind the phone keypad, so phone keys
reach the shell only through the terminal. Keyboards type through the VT and
its keymap and need no console support.

The terminal uses `TERM=linux`. It keeps shell output and console input on the
primary virtual terminal, so a port must not substitute an escape-sequence
overlay for `fbcon`. The user-visible keypad behavior belongs to the
[local console feature](../features/LOCAL_CONSOLE.md), not to each target port.

## Target responsibilities

A console target provides, itself or through its platform:

- its framebuffer or DRM driver and the mode exposed through `fbcon`;
- keypad scan, wiring and the keymap to the phone key codes;
- DTS and built-in kernel configuration required for evdev, framebuffer console,
  virtual terminals and the shell's PTY support;
- when used, the host-keyboard bridge's uinput and generic-serial dependencies.

The target's support document states which of these interfaces has been
exercised on its physical hardware. See the [target template](TARGET.md) and
the selected platform document for board-specific requirements.

See the [porting overview](README.md) for the complete layer boundary and the
project [documentation index](../../README.md#documentation) for shared guides,
features and applications.
