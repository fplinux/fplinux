# Console port contract

FPLinux splits a console phone into a shared post-kernel terminal stack and
phone-specific hardware support.

## Shared userspace

The shared local terminal is built by the `fplinux-console` Buildroot package.
It requires ordinary Linux interfaces only:

- stdin and stdout refer to the same active Linux VT in text mode, backed by
  `fbcon`;
- `/dev/tty0`, matching writable `/dev/vcsaN` character devices (Linux major
  `7`, minor `128 + N`), and a spare VT returned by `VT_OPENQRY` are available;
- `/dev/input` may expose one primary and up to three additional evdev devices
  with the normalized keypad capabilities listed below; missing input devices
  are nonfatal;
- `/dev/ptmx` and `devpts` are available for the interactive shell;
- `TIOCGWINSZ` reports non-zero, bounded rows and columns matching the active
  VCSA device.

The terminal derives the primary VT from stdin/stdout when they name a specific
`/dev/ttyN`. It also resolves both descriptors with `TIOCGDEV` and requires
them to match the device node for the active VT reported by `VT_GETSTATE`.
Missing VT, VCSA, spare-VT, geometry, or write capabilities are fatal at
startup; stdout escape overlays are not a fallback porting API.

The bottom physical row is an inverse-video status bar. The shell PTY receives
one row less than the physical VT, and a Linux scrolling region keeps shell
output above the bar. It displays `T9` or `QWERTY` plus the active one-shot
`CTRL`, `ALT` or `SHIFT` modifier. The bar is written through VCSA because even
a saved and restored cursor move cancels a pending right-margin wrap.

The terminal uses `TERM=linux` and a bounded input FIFO. Shell PTY output is
forwarded to the primary VT byte-for-byte in bounded read batches so a
continuously writing child cannot starve signals, input discovery or monotonic
deadlines. A multi-tap candidate exists in the VCSA cursor cell only while the
terminal is blocked in `poll`; it is removed before any shell output is written.
There is no output transformation or stdout-overlay fallback.

### Normalized keypad UX

The required evdev capability set is `KEY_0` through `KEY_9`, `KEY_TAB`,
`KEY_BACKSPACE`, `KEY_ENTER`, `KEY_KPASTERISK`, `KEY_KPDOT`, and all four arrow
keys. Discovery safely enumerates bounded `/dev/input/event*` entries and
selects by capabilities, not event number or hardware identity. It opens one
primary and up to three additional matching devices, setting their repeat delay
to 400 ms and period to 40 ms. A missing device is nonfatal, and the continuous
discovery loop also replaces removed devices.

The default `T9` label names multi-tap composition; there is no dictionary or
prediction. Digits compose entirely in userspace, and no provisional candidate
bytes enter the shell PTY:

- `2` through `9`: `abc2`, `def3`, `ghi4`, `jkl5`, `mno6`, `pqrs7`, `tuv8`,
  and `wxyz9`;
- `1`: punctuation and symbols, followed by digit `1`;
- `0`: space, then digit `0`.

The first press displays a temporary reverse/blinking candidate in the current
VCSA cursor cell. Repeated presses of the same digit cycle that cell. Timeout
commits it; another digit commits it and starts a new candidate. Soft-right
cancels an active candidate without sending Delete, and otherwise sends DEL.
Enter commits and appends CR in one FIFO operation. An arrow commits and appends
the corresponding unmodified Linux-console arrow sequence in one FIFO operation.

A short `*` first commits a candidate, then cycles the one-shot selector
`Ctrl -> Alt -> Shift -> none`. The active selector appears in the status bar
and as a temporary inline VCSA marker while the terminal is blocked in `poll`. At character commit, Shift uppercases lowercase
ASCII, Alt prefixes ESC, and Ctrl maps ASCII letters or `@` through `_` with
`c & 0x1f`; Ctrl-`?` emits DEL. Unsupported Ctrl characters are cancelled with a
temporary visual marker and enqueue no bytes. A modifier resets after one
successfully committed composed character. Shift plus left-soft emits ESC then
Tab and consumes Shift. Ctrl/Alt plus left-soft are rejected with the same
visual marker. Modifiers never invent arrow-key sequences outside `TERM=linux`.

Holding `*` for 400 ms switches between `T9` and `QWERTY`. QWERTY enables the
Linux console keymap with `K_XLATE`, and the terminal forwards the translated
bytes from VT stdin without interpreting them. Holding `*` again returns to
T9. Releasing an ordinary typewriter key while T9 is active also selects
QWERTY; that switching key itself does not reach the shell.

Evdev autorepeat (`value == 2`) is accepted for arrows and soft-right Backspace,
and ignored for digits, `*`, `#`, Enter, and left-soft Tab.

### History VT

`#` commits an active candidate and switches to the spare Linux VT while the
primary VT, shell PTY, and byte-exact forwarding remain live. History is a
bounded, sanitized, line-oriented transcript of shell PTY output managed by
`fplinux-console`. It starts with output forwarded after the console-managed
shell begins, not earlier boot `dmesg`, and it is not a reconstruction of
full-screen TUIs. Untrusted terminal control strings are never replayed on the
history VT; rendering writes sanitized cells directly through `/dev/vcsaN`.

In history, Up/Down move one transcript line, Left/Right move one page, OK jumps
to the newest page, and `#` or soft-right returns to the primary VT. Other
physical keys do not reach the shell. Kernel keyboard translation is disabled on
the spare VT, leaving evdev as its only input path and preventing line-discipline
echo from overwriting the VCSA-rendered history. Shutdown returns to the primary
VT when necessary and disallocates the spare VT.

The common init reads branding from `/etc/os-release`, starts `usb-getty` and
`fplinux-input`, then runs `/bin/fplinux-console`. `usb-getty` gives
`/dev/ttyGS0` to BusyBox `getty` as the USB shell's controlling terminal.
`fplinux-input` reads `type code value` lines from `/dev/ttyGS1`, creates the
`FPLinux host keyboard` uinput device and keeps that device alive across host
disconnections.

The console takes a record lock under `/tmp` before changing its VT. A second
copy on the same VT is refused, and the kernel releases the lock when its owner
dies. Failure to create or take the lock for any other reason is reported but
does not remove the phone's local console. A target may install this executable
hook:

```text
/usr/libexec/fplinux/boot-diagnostics
```

The hook may print a bounded phone-specific boot summary. Background USB
readiness messages go to `/dev/kmsg`, so they cannot corrupt the local VT.

## Target responsibilities

A phone target still owns:

- the framebuffer or DRM driver and the mode exposed to `fbcon`;
- keypad scan, wiring and conversion to the normalized Linux input key codes;
- DTS nodes and kernel configuration, including evdev, uinput, file locking and
  two generic-serial gadget ports when the host keyboard bridge is enabled;
- the bootstrap board descriptor and target payload assembly;
- the memory map, board assets and validated platform-adapter values;
- `/etc/os-release` and an optional boot diagnostics hook.

The selected platform's fixed adapter owns the host loader sequence. A target
supplies board data to that sequence but cannot select commands or executable
paths.

A pre-Linux screen that accesses a fixed framebuffer or inherited panel state is
not part of the shared userspace API. Phones on one SoC may share their screen
instance, hardware adapter and boot sequencing under `platforms/<soc>/bootstrap/`.
Each target then supplies only its board descriptor and payload assembly under
`targets/<phone>/bootstrap/`. The platform flow may compile the platform-neutral
font, rasterizer and layout from
[`bootstrap/fplinux-boot-screen/`](../../bootstrap/fplinux-boot-screen/) into the
pinned bootstrap closure.

## Root filesystem overlays

Apply the common overlay first and the target overlay second:

```text
BR2_ROOTFS_OVERLAY="/workspace/common/rootfs/overlay /workspace/targets/<phone>/rootfs/overlay"
```

The target overlay may add identity and diagnostics files. It must not fork the
shared terminal implementation to change keypad scan codes; normalize those in
the target input driver instead.
