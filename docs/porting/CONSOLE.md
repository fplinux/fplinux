# Console port contract

FPLinux's local terminal is shared userspace. A phone port supplies standard
Linux display and input interfaces; the terminal does not contain panel-register
or scan-code knowledge.

## Required interfaces

The local terminal needs:

- a usable text virtual terminal backed by `fbcon`;
- `/dev/ptmx` and `devpts` for the interactive shell;
- at least one evdev keypad with the normalized keys below.

The terminal discovers compatible evdev devices by capability, not event number
or phone identity. Additional compatible input devices are optional. A target
with the host-keyboard bridge also provides its designated generic-serial input
path and a persistent uinput keyboard device.

## Normalized keypad UX

The keypad driver reports normal Linux input codes. The console requires digits
`KEY_0` through `KEY_9`, `KEY_TAB`, `KEY_BACKSPACE`, `KEY_ENTER`,
`KEY_KPASTERISK`, `KEY_KPDOT`, and the four arrow keys. Target keymaps choose
which physical keys provide them.

The terminal starts in T9 multi-tap mode; T9 is composition, not dictionary
prediction. Its visible behaviour is:

- the bottom row shows `T9` or `QWERTY` and an armed one-shot `CTRL`, `ALT` or
  `SHIFT` modifier;
- repeated digit presses cycle a character, while `1` selects punctuation and
  `0` selects space;
- a short `*` cycles the one-shot modifier; holding `*` switches between T9
  and QWERTY;
- Enter and arrow keys commit pending composition before acting; right soft
  cancels it or sends Backspace; left soft sends Tab;
- `#` switches to and from the terminal's scrollback view without stopping the
  shell;
- QWERTY uses the Linux console keymap and sends its translated input to the
  shell.

The terminal uses `TERM=linux`. It keeps shell output and console input on the
primary virtual terminal, so a port must not substitute an escape-sequence
overlay for `fbcon`.

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
