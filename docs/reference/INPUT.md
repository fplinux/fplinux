# Input contract

Phone applications read keyboards, mice and the phone keypad through one shared
input stack. This page defines what an application or port relies on.

## Devices

On a supported target, Bluetooth keyboards and mice and keyboards forwarded by
the [USB keyboard bridge](../features/HOST_KEYBOARD.md) appear as Linux evdev
devices. The phone keypad has the evdev `phys` string `fplinux/keypad0` on every
target.

Phone keys use standard Linux input codes. The aliases in
`include/fplinux/fplinux-keypad.h` give them phone roles only on a device whose
`phys` string is `fplinux/keypad0`:

| Phone key                | Linux code                                    |
| ------------------------ | --------------------------------------------- |
| Digits `0` to `9`        | `KEY_NUMERIC_0` to `KEY_NUMERIC_9`            |
| `*` and `#`              | `KEY_NUMERIC_STAR`, `KEY_NUMERIC_POUND`       |
| D-pad                    | `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT` |
| Centre                   | `KEY_OK`                                      |
| Left and right soft keys | `KEY_F13`, `KEY_F14`                          |
| Dial                     | `KEY_PICKUP_PHONE`                            |
| Power                    | `KEY_POWER`                                   |

External keyboard F13 and F14 remain keyboard function keys. A key code alone
never identifies a phone action. Holding the phone power key for five seconds
requests power-off even while an application grabs the keypad.

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
rebroadcasts kernel device events and leaves `/dev` to devtmpfs. The shared input stack does not deliver kernel autorepeat; consumers such as
the terminal generate their own repeat behavior. A suspended graphical session
releases its input grabs and clears pressed-key state before resuming.

## Keyboard layouts

Keyboard layout data is XKB source under `/usr/share/fplinux/xkb`. The
`fplinux-xkb` package, part of every system root, provides the US layout and
the files that layouts share. Each further layout is its own optional APK that
installs `symbols/<layout>` and an empty registration file `layouts/<layout>`;
`fplinux-xkb-ru` adds Russian. A layout package installs only its own files and
depends on `fplinux-xkb`.

An application builds its keymap from the components
`evdev+aliases(qwerty)`, `complete` types and compatibility, and the symbols
`pc+us+fplinux(function_keys)`, followed by `+<layout>:<n>` for each registered layout in name order,
and `+group(alt_shift_toggle)`. The data root is the only include path.
Alt+Shift switches between the layouts. XKB allows four layouts, so at most
three can be registered beside US; with more, or with a registration name other
than lowercase letters, digits and `_`, the application refuses to start and
names the problem. The [local terminal](../features/LOCAL_CONSOLE.md) and
[MicroPythonOS](../apps/MICROPYTHONOS.md) use these installed layouts for
physical-keyboard text through the shared libxkbcommon library.
