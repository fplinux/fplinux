# Input contract

Phone applications read keyboards, mice and the phone keypad through one shared
input stack. This page defines what an application or port relies on.

## Devices

On a supported target, Bluetooth keyboards and mice and keyboards forwarded by
the [USB keyboard bridge](../features/HOST_KEYBOARD.md) appear as Linux evdev
devices. The phone keypad has the evdev `phys` string `fplinux/keypad0` on every
target.

Every phone key reports its own code, defined in
`include/fplinux/fplinux-keypad.h`. These codes are outside the ordinary
keyboard keymap, and the kernel console does not translate them. Applications
identify the phone keypad by its `phys` string, rather than assuming that a
key code alone identifies its source:

| Phone key         | Code                                                                |
| ----------------- | ------------------------------------------------------------------- |
| Digits `0` to `9` | `FPLINUX_KEY_0` to `FPLINUX_KEY_9`, `0x1c4` to `0x1cd`              |
| `*` and `#`       | `FPLINUX_KEY_STAR` `0x1ce`, `FPLINUX_KEY_POUND` `0x1cf`             |
| D-pad             | `FPLINUX_KEY_UP`, `_DOWN`, `_LEFT` and `_RIGHT`, `0x233` to `0x236` |
| Centre key        | `FPLINUX_KEY_OK` `0x237`                                            |
| Soft keys         | `FPLINUX_KEY_SOFT_LEFT` `0x238`, `FPLINUX_KEY_SOFT_RIGHT` `0x239`   |
| Dial (green) key  | `FPLINUX_KEY_CALL` `0x23a`                                          |
| Power key         | `FPLINUX_KEY_POWER` `0x23b`                                         |

Holding the power key for five seconds requests power-off, even while an
application grabs the keypad. Further phone keys take the reserved codes
`0x23c` to `0x23f`, then `0x1e6` to `0x1f0`.

Applications open devices through the shared `fplinux-input-session` component
in `lib/fplinux`. It uses libinput over `fplinux-libudev` and classifies each
device by its keypad `phys` string and capabilities, never by its bus:

| Source   | Device                                                                |
| -------- | --------------------------------------------------------------------- |
| Keypad   | The phone keypad                                                      |
| Keyboard | Any other device with an `Enter` key, even if it also moves a pointer |
| Pointer  | Otherwise, relative X and Y motion with a left button                 |

A mouse that also publishes a separate key-only device contributes that device
as a keyboard if it reports `Enter`. An application chooses the sources it
accepts; accepted devices are grabbed for the life of its session, so their
keys do not reach the local console. Devices that connect later are added and
devices that disconnect are removed, with their pressed keys released first.
The `fplinux-mdevd` service provides these hotplug notifications; it only
rebroadcasts kernel device events and leaves `/dev` to devtmpfs. Key
autorepeat is not delivered.

## Keyboard layouts

Keyboard layout data is XKB source under `/usr/share/fplinux/xkb`. The
`fplinux-xkb` package, part of every system root, provides the US layout and
the files that layouts share. Each further layout is its own optional APK that
installs `symbols/<layout>` and an empty registration file `layouts/<layout>`;
`fplinux-xkb-ru` adds Russian. A layout package installs only its own files and
depends on `fplinux-xkb`.

An application builds its keymap from the components
`evdev+aliases(qwerty)`, `complete` types and compatibility, and the symbols
`pc+us`, followed by `+<layout>:<n>` for each registered layout in name order,
and `+group(alt_shift_toggle)`. The data root is the only include path.
Alt+Shift switches between the layouts. XKB allows four layouts, so at most
three can be registered beside US; with more, or with a registration name other
than lowercase letters, digits and `_`, the application refuses to start and
names the problem. [MicroPythonOS](../apps/MICROPYTHONOS.md)
uses the installed layouts for text typed on a physical keyboard.
