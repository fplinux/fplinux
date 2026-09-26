# MicroPythonOS

MicroPythonOS is an optional graphical MicroPython environment for the local
FPLinux framebuffer and physical keypad. See the
[target documentation](../../targets/README.md) for its support and display
limits on the selected phone.

The package is installed separately into the active system root; it is not
preinstalled in the normal root filesystem. Installation lasts until shutdown
in the `default` RAM profile and persists across boots in `microsd-uboot`.
First load or reconnect
to the selected phone using its instructions. A standalone archive includes
this page; start with its
top-level `README.txt`.

## Included applications

The launcher, How-to, About, file manager, and Settings are built in. The
installed package also includes:

- Text editor, Image Viewer, and REPL Shell;
- Hello World, Connect Four, Lights Out, Memory, Columns, and Flood-It;
- Show Fonts; and
- Keypad Test, which demonstrates numeric-keypad text entry.

The target's supported display, keypad, and storage determine what these
applications can use. Features that require an unsupported phone service are
not made available by FPLinux.

## Install

All three targets use the same `fplinux-micropythonos.apk` file. Its installed
package name is `fplinux-micropythonos`. Follow
[Installing and removing optional APK packages](../guides/APK_PACKAGES.md) for
either a source checkout or a standalone archive.

The source-checkout example below uses `inoi-244-modern-4g` with the `default`
profile. Replace both values with the target and profile of the running phone.

### Optional FAT32 data-card configuration

After installing the base package, install `fplinux-micropythonos-storage.apk`
to select the optional FAT32 data-card path. This shared configuration does
not enable a missing microSD driver; check the selected phone's storage support.
Its installed package name is `fplinux-micropythonos-storage`; use the same
shared installation procedure after the base package is installed.

## Run

Open an interactive shell and start the application on the phone:

```sh
# Source checkout
./fplinux console inoi-244-modern-4g --profile default

# Standalone archive
./runner/run.py --reconnect
```

At the phone shell prompt, run:

```sh
micropythonos
```

Press `Ctrl-C` in the shell when finished. It stops MicroPythonOS and returns
to the terminal.

## Use the keypad

MicroPythonOS uses the physical phone keypad on each supported target.
Keyboards can be used at the same time; see
[Use a keyboard](#use-a-keyboard).

| Key            | Action                                                        |
| -------------- | ------------------------------------------------------------- |
| D-pad          | Move focus; operate open lists, drop-downs, and text controls |
| Centre or dial | Select a focused item or confirm text input                   |
| Left soft      | Open or close the top settings and notification drawer        |
| Right soft     | Return to the previous screen or finish active text entry     |
| `0` to `9`     | Enter text with multi-tap when a text field is active         |
| `*`            | Cancel the pending character or erase the previous character  |
| `#`            | Switch lower- and upper-case text entry                       |

For multi-tap, press the same number repeatedly to cycle its characters. A
character is committed after a short pause, when another number is pressed, or
when `Centre` or dial is pressed. The Keypad Test application shows the active
case mode and current candidate.

## Use a keyboard

MicroPythonOS reads keyboards together with the phone keypad: the keyboard
forwarded through the [host keyboard bridge](../features/HOST_KEYBOARD.md), and
Bluetooth keyboards paired with the phone following
[Keyboards and mice](../features/BLUETOOTH.md#keyboards-and-mice). The
target's documentation states whether it supports Bluetooth. A keyboard can
connect before or while MicroPythonOS runs. While it runs, MicroPythonOS has
exclusive use of the phone keypad and every keyboard, so their keys do not
reach the local console.

Letters, digits, punctuation and space are typed literally into the focused
text field; keyboard digits never start multi-tap there. A pending multi-tap
character is committed before the typed text. Other keys act like phone keys:

| Key         | Action                                                             |
| ----------- | ------------------------------------------------------------------ |
| Arrow keys  | Same as the D-pad                                                  |
| `Enter`     | Same as `Centre` or dial                                           |
| `Tab`       | Same as Left soft                                                  |
| `Esc`       | Same as Right soft                                                 |
| `Backspace` | Erase the previous character in a text field; otherwise Right soft |

Keys do not repeat while held.

The US layout is always available. Each optional layout package adds another
layout, and `Alt+Shift` switches to the next one. The Russian layout is in
`fplinux-xkb-ru.apk`, with the installed package name `fplinux-xkb-ru`; install
or remove it with the shared [APK package instructions](../guides/APK_PACKAGES.md)
and then restart MicroPythonOS. At most three optional layouts can be installed;
with more, MicroPythonOS stops at startup and reports the error.

The bundled MicroPythonOS fonts do not contain Cyrillic glyphs. Russian text is
entered into the field, but it is not shown on the screen.

## Storage

Without a usable target storage path, MicroPythonOS keeps its apps, cache,
data, libraries, and preferences under `/var/lib/micropythonos` on the active
system root. This state is discarded when the `default` RAM session ends and
persists across boots with the `microsd-uboot` ext4 root.

See the selected phone's instructions for its current screen and storage
support.

The optional storage package can use a supported FAT32 microSD card that is
already mounted at `/mnt/card`. It stores state under
`/mnt/card/.fplinux/micropythonos` and falls back to `/var/lib/micropythonos`
otherwise. In `microsd-uboot`, state stays under `/var/lib/micropythonos` on the
active ext4 root; the root is not automatically mounted at `/mnt/card`.

MicroPythonOS never mounts or unmounts a card itself. Mount the data card first
with the [microSD instructions](../features/MICROSD.md); the application then
uses `/mnt/card` only when that mount is the declared card filesystem, and keeps
its state on the system root in every other case. Never remove a mounted card;
follow the selected phone's microSD instructions for safe unmount and hot-swap
of a data card. For a system card, follow
[microSD root shutdown](../guides/MICROSD_ROOT.md#persistence-and-shutdown);
it cannot be hot-swapped while Linux is running.

## Remove

Stop MicroPythonOS with `Ctrl-C`, exit the interactive shell, then remove the
package or packages from the active system root. Follow the shared
[package removal instructions](../guides/APK_PACKAGES.md#remove-a-package).
Remove `fplinux-micropythonos-storage` if it is installed, then remove
`fplinux-micropythonos`.
