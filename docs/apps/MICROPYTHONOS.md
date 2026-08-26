# MicroPythonOS

MicroPythonOS is an optional graphical MicroPython environment for the local
FPLinux framebuffer and physical keypad. It is installed into the current RAM
session; it is not part of the normal root filesystem. First load or reconnect
to the selected phone using its instructions. A standalone archive includes
this page; start with its top-level `README.txt`.

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

All current targets require the base
`fplinux-micropythonos.apk` package. Nokia 3210 4G (TA-1618) also requires its
companion package; use the Nokia instructions below instead of the base-only
install.

### Base package: source checkout

Set `target` to the running target and `bundle` to the `output:` directory from
its build, then upload and install the base APK:

```sh
target=inoi-244-modern-4g
bundle=/absolute/path/printed-by-fplinux-build

./fplinux console "$target" --upload \
  "$bundle/apks/fplinux-micropythonos.apk" /tmp/fplinux-micropythonos.apk
./fplinux console "$target" --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk'
```

### Base package: standalone archive

From the extracted archive directory:

```sh
./runner/run.py --reconnect --upload \
  ./apks/fplinux-micropythonos.apk /tmp/fplinux-micropythonos.apk
./runner/run.py --reconnect --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk'
```

### Nokia 3210 4G (TA-1618) companion

On TA-1618, upload both APKs and install them together. The companion declares
the optional microSD storage path used by this phone.

```sh
# Source checkout
target=nokia-ta1618
bundle=/absolute/path/printed-by-nokia-build

./fplinux console "$target" --upload \
  "$bundle/apks/fplinux-micropythonos.apk" /tmp/fplinux-micropythonos.apk
./fplinux console "$target" --upload \
  "$bundle/apks/fplinux-micropythonos-nokia-ta1618.apk" \
  /tmp/fplinux-micropythonos-nokia-ta1618.apk
./fplinux console "$target" --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk /tmp/fplinux-micropythonos-nokia-ta1618.apk'

# Standalone archive
./runner/run.py --reconnect --upload \
  ./apks/fplinux-micropythonos.apk /tmp/fplinux-micropythonos.apk
./runner/run.py --reconnect --upload \
  ./apks/fplinux-micropythonos-nokia-ta1618.apk \
  /tmp/fplinux-micropythonos-nokia-ta1618.apk
./runner/run.py --reconnect --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk /tmp/fplinux-micropythonos-nokia-ta1618.apk'
```

Follow that phone's microSD instructions.

## Run

Open an interactive shell and start the application on the phone:

```sh
# Source checkout
./fplinux console "$target"

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

MicroPythonOS uses the normalized physical phone keypad on every current
target. The host-keyboard bridge does not control MicroPythonOS.

| Key            | Action                                                        |
| -------------- | ------------------------------------------------------------- |
| D-pad          | Move focus; operate open lists, drop-downs, and text controls |
| Centre or dial | Select a focused item or confirm text input                   |
| Left soft      | Open or close the application drawer                          |
| Right soft     | Return to the previous screen or finish active text entry     |
| `0` to `9`     | Enter text with multi-tap when a text field is active         |
| `*`            | Cancel the pending character or erase the previous character  |
| `#`            | Switch lower- and upper-case text entry                       |

For multi-tap, press the same number repeatedly to cycle its characters. A
character is committed after a short pause, when another number is pressed, or
when `Centre` or dial is pressed. The Keypad Test application shows the active
case mode and current candidate.

## Storage

Without a usable target storage path, MicroPythonOS keeps its apps, cache,
data, libraries, and preferences in RAM. That state is discarded when the RAM
session ends.

The INOI targets have no supported microSD path, so state remains in RAM. On the
INOI 240 Modern 4G, its `128×160` display clips launcher content and some
application controls. See the selected phone's instructions for its current
screen and storage support.

On TA-1618, the companion package can use a FAT32 microSD card at `/mnt/card`.
It stores state under `/mnt/card/.fplinux/micropythonos` and falls back to RAM
without a usable card. MicroPythonOS mounts a declared card only when
`/mnt/card` is not already mounted. It unmounts only a card that it mounted
itself when the application exits. Never remove a mounted card; follow the
selected phone's microSD instructions for safe unmount and hot-swap.

## Remove

Stop MicroPythonOS with `Ctrl-C`, exit the interactive shell, then remove the
package or packages from the current session:

```sh
# Source checkout, INOI targets
./fplinux console "$target" --exec 'apk del fplinux-micropythonos'

# Source checkout, Nokia TA-1618
./fplinux console "$target" --exec \
  'apk del fplinux-micropythonos-nokia-ta1618 fplinux-micropythonos'

# Standalone archive, INOI targets
./runner/run.py --reconnect --exec 'apk del fplinux-micropythonos'

# Standalone archive, Nokia TA-1618
./runner/run.py --reconnect --exec \
  'apk del fplinux-micropythonos-nokia-ta1618 fplinux-micropythonos'
```
