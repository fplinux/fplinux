# MicroPythonOS

MicroPythonOS is an optional graphical MicroPython environment for the local
FPLinux framebuffer and physical keypad. It runs on Nokia TA-1618 and INOI 244
Modern 4G. It also launches on INOI 240 Modern 4G, with partial support because
the UI is not fully adapted to its `128×160` screen.

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

All three targets use the same `fplinux-micropythonos.apk` package.

### Source checkout

Set `target` and `profile` to match the running system and `bundle` to the
`output:` directory from its build, then upload and install the base APK:

```sh
target=inoi-244-modern-4g
profile=default
bundle=/absolute/path/printed-by-fplinux-build

./fplinux console "$target" --profile "$profile" --upload \
  "$bundle/apks/fplinux-micropythonos.apk" /tmp/fplinux-micropythonos.apk
./fplinux console "$target" --profile "$profile" --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk'
```

### Standalone archive

From the extracted archive directory:

```sh
./runner/run.py --reconnect --upload \
  ./apks/fplinux-micropythonos.apk /tmp/fplinux-micropythonos.apk
./runner/run.py --reconnect --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk'
```

### Optional FAT32 data-card configuration

After installing the base package, install `fplinux-micropythonos-storage.apk`
to select the optional FAT32 data-card path. This shared configuration does
not enable a missing microSD driver; check the selected phone's storage support.

```sh
# Source checkout
./fplinux console "$target" --profile "$profile" --upload \
  "$bundle/apks/fplinux-micropythonos-storage.apk" /tmp/fplinux-micropythonos-storage.apk
./fplinux console "$target" --profile "$profile" --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos-storage.apk'

# Standalone archive
./runner/run.py --reconnect --upload \
  ./apks/fplinux-micropythonos-storage.apk /tmp/fplinux-micropythonos-storage.apk
./runner/run.py --reconnect --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos-storage.apk'
```

## Run

Open an interactive shell and start the application on the phone:

```sh
# Source checkout
./fplinux console "$target" --profile "$profile"

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

MicroPythonOS uses the normalized physical phone keypad on each supported
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
data, libraries, and preferences under `/var/lib/micropythonos` on the active
system root. This state is discarded when the `default` RAM session ends and
persists across boots with the `microsd-uboot` ext4 root.

See the selected phone's instructions for its current screen and storage
support.

The optional storage package can use a supported FAT32 microSD card at `/mnt/card`.
It stores state under `/mnt/card/.fplinux/micropythonos` and falls back to
`/var/lib/micropythonos` without a usable declared card. MicroPythonOS skips
the boot partition labelled `FPLBOOT` during automatic mounting. In
`microsd-uboot`, state stays under `/var/lib/micropythonos` on the active ext4
root; the root is not automatically mounted at `/mnt/card`.

MicroPythonOS mounts a declared data card only when `/mnt/card` is not already
mounted. It unmounts only a card that it mounted itself when the application
exits. Never remove a mounted card; follow the
selected phone's microSD instructions for safe unmount and hot-swap of a data
card. For a system card, follow
[microSD root shutdown](../guides/MICROSD_ROOT.md#persistence-and-shutdown);
it cannot be hot-swapped while Linux is running.

## Remove

Stop MicroPythonOS with `Ctrl-C`, exit the interactive shell, then remove the
package or packages from the active system root:

```sh
# Source checkout, base package only
./fplinux console "$target" --profile "$profile" --exec 'apk del fplinux-micropythonos'

# Source checkout, with the optional storage package
./fplinux console "$target" --profile "$profile" --exec \
  'apk del fplinux-micropythonos-storage fplinux-micropythonos'

# Standalone archive, base package only
./runner/run.py --reconnect --exec 'apk del fplinux-micropythonos'

# Standalone archive, with the optional storage package
./runner/run.py --reconnect --exec \
  'apk del fplinux-micropythonos-storage fplinux-micropythonos'
```
