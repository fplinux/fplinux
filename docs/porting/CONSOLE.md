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
- one evdev device reports the normalized keypad keys listed below;
- `/dev/ptmx` and `devpts` are available for the interactive shell;
- `TIOCGWINSZ` reports non-zero, bounded rows and columns matching the active
  VCSA device.

The terminal derives the primary VT from stdin/stdout when they name a specific
`/dev/ttyN`. It also resolves both descriptors with `TIOCGDEV` and requires
them to match the device node for the active VT reported by `VT_GETSTATE`.
Missing VT, VCSA, spare-VT, geometry, or write capabilities are fatal at
startup; stdout escape overlays are not a fallback porting API.

The shell PTY receives the complete physical VT geometry, uses `TERM=linux`, and
keeps a bounded input FIFO. Shell PTY output is forwarded to the primary VT
byte-for-byte in bounded read batches so a continuously writing child cannot
starve signals, keypad discovery, input, or monotonic deadlines. Before a batch
is forwarded, the terminal conditionally removes its VCSA overlay only when the
cell still contains the terminal's exact marker; after the original bytes are
written unchanged, it reads the new cursor and re-anchors any active overlay.
The ownership check and restore are separate VCSA reads and writes, not an
atomic compare-and-restore operation. Asynchronous external VT writers can race
either operation, so overlay ownership is best-effort. There is no stdout
escape-overlay fallback, custom output transformation, or persistent status row
in this path.

### Normalized keypad UX

The required evdev capability set is `KEY_0` through `KEY_9`, `KEY_TAB`,
`KEY_BACKSPACE`, `KEY_ENTER`, `KEY_KPASTERISK`, `KEY_KPDOT`, and all four arrow
keys. Discovery safely enumerates bounded `/dev/input/event*` entries and
selects by capabilities, not by event number or phone identity. A missing keypad
at startup is nonfatal: the PTY and shell remain live while discovery retries on
a timed monotonic schedule. The same continuous discovery resumes if the
selected evdev device is removed later.

Digits compose entirely in userspace; no provisional candidate bytes enter the
shell PTY:

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

`*` first commits a candidate, then cycles the one-shot selector
`Ctrl -> Alt -> Shift -> none`. The selector is a temporary inline VCSA marker,
not a permanent indicator. At character commit, Shift uppercases lowercase
ASCII, Alt prefixes ESC, and Ctrl maps ASCII letters or `@` through `_` with
`c & 0x1f`; Ctrl-`?` emits DEL. Unsupported Ctrl characters are cancelled with a
temporary visual marker and enqueue no bytes. A modifier resets after one
successfully committed composed character. Shift plus left-soft emits ESC then
Tab and consumes Shift. Ctrl/Alt plus left-soft are rejected with the same
visual marker. Modifiers never invent arrow-key sequences outside `TERM=linux`.

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

The common init reads branding from `/etc/os-release`, starts the external
`usb-getty` worker and then runs `/bin/fplinux-console`. When `/dev/ttyGS0`
exists, the worker runs BusyBox `getty`, which creates the USB shell session and
makes the gadget tty its controlling terminal. A target may install this
executable hook:

```text
/usr/libexec/fplinux/boot-diagnostics
```

The hook may print a bounded phone-specific boot summary. Background USB
readiness messages go to `/dev/kmsg`, so they cannot corrupt the local VT.

## Target responsibilities

A phone target still owns:

- the framebuffer or DRM driver and the mode exposed to `fbcon`;
- keypad scan, wiring and conversion to the normalized Linux input key codes;
- DTS nodes and kernel configuration;
- pre-Linux screen composition, framebuffer/panel adapter and boot sequencing;
- bootstrap, memory map, board assets and validated platform-adapter values;
- `/etc/os-release` and an optional boot diagnostics hook.

The selected platform's fixed adapter owns the host loader sequence. A target
supplies board data to that sequence but cannot select commands or executable
paths.

A pre-Linux screen that accesses a fixed framebuffer or inherited panel state is
not part of the shared userspace API. Keep the screen instance, hardware adapter
and bootstrap sequencing under `targets/<phone>/bootstrap/`. Targets may compile
the platform-neutral font, rasterizer and layout from
[`bootstrap/fplinux-boot-screen/`](../../bootstrap/fplinux-boot-screen/) into
their pinned bootstrap closure.

## Root filesystem overlays

Apply the common overlay first and the target overlay second:

```text
BR2_ROOTFS_OVERLAY="/workspace/common/rootfs/overlay /workspace/targets/<phone>/rootfs/overlay"
```

The target overlay may add identity and diagnostics files. It must not fork the
shared terminal implementation to change keypad scan codes; normalize those in
the target input driver instead.
