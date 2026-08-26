# TyrQuake

TyrQuake 0.71 is an optional native game for the current FPLinux RAM session.
The APK contains the engine, not Quake game data. Provide a legally obtained
`pak0.pak` before starting the game. First load or reconnect to the selected
phone using its instructions.

## Install

The package is not part of the normal root filesystem. Installing it changes
only the current RAM session.

### Source checkout

Set `target` to the running target and `bundle` to the `output:` directory from
its build, then upload and install the APK:

```sh
target=inoi-244-modern-4g
bundle=/absolute/path/printed-by-fplinux-build

./fplinux console "$target" --upload \
  "$bundle/apks/fplinux-tyrquake.apk" /tmp/fplinux-tyrquake.apk
./fplinux console "$target" --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-tyrquake.apk'
```

### Standalone archive

From the extracted archive directory, use its bundled APK:

```sh
./runner/run.py --reconnect --upload \
  ./apks/fplinux-tyrquake.apk /tmp/fplinux-tyrquake.apk
./runner/run.py --reconnect --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-tyrquake.apk'
```

## Game data

The launcher reads `pak0.pak` from this exact path:

```text
/mnt/card/fplinux/quake/id1/pak0.pak
```

It also accepts readable `pak1.pak` through `pak9.pak` from that directory.
The game data is never copied into the APK or its temporary runtime directory.

### Nokia 3210 4G (TA-1618)

Use a FAT32 microSD card. Follow the TA-1618 target document to mount the card,
make `fplinux/quake/id1`, and upload the PAK. Before removing the card, run its
documented `sync` and `umount` procedure.

When `/mnt/card` is not already mounted, `quake` mounts the card read-only for
the game. Game data remains on the card, but the game never writes saves or
settings there.

### INOI 240 Modern 4G and INOI 244 Modern 4G

Neither target has a supported microSD path. Create a temporary mount and copy
the PAK into it for this one RAM session:

```sh
# Source checkout
./fplinux console "$target" --exec \
  'mkdir -p /mnt/card && mount -t tmpfs tmpfs /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
./fplinux console "$target" --upload \
  ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak

# Standalone archive
./runner/run.py --reconnect --exec \
  'mkdir -p /mnt/card && mount -t tmpfs tmpfs /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
./runner/run.py --reconnect --upload \
  ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak
```

The PAK consumes phone RAM and disappears with the session. A full PAK leaves
less room for the game; see the selected INOI target document for its memory
limit.

## Run

Choose one input mode. `phone` uses the physical keypad. `keyboard` uses the
host keyboard forwarded through the USB keyboard bridge; it does not combine
with the phone keypad. Start forwarding first as described in
[Host keyboard forwarding](../features/HOST_KEYBOARD.md).

```sh
# Source checkout
./fplinux console "$target" --exec 'quake --input phone'
./fplinux console "$target" --exec 'quake --input keyboard'

# Standalone archive
./runner/run.py --reconnect --exec 'quake --input phone'
./runner/run.py --reconnect --exec 'quake --input keyboard'
```

The command keeps the phone display in game mode until TyrQuake exits.

## Controls

In phone mode, turn the phone counter-clockwise: the display is on the left
and the keypad is on the right. This mapping is the same on every current
target.

| Key                    | Menu                 | Game                        |
| ---------------------- | -------------------- | --------------------------- |
| D-pad `UP` / `DOWN`    | Left / right         | Turn left / right           |
| D-pad `LEFT` / `RIGHT` | Down / up            | Walk backward / forward     |
| Centre or dial         | Select               | Fire                        |
| Right soft             | Back                 | Menu                        |
| Left soft or `*`       | —                    | Jump                        |
| `0`                    | —                    | Fire                        |
| `1` / `3`              | —                    | Strafe left / right         |
| `2` / `5`              | —                    | Turn left / right           |
| `4` / `6`              | —                    | Walk backward / forward     |
| `7` / `9`              | —                    | Previous / next weapon      |
| `8`                    | —                    | Run while held              |
| `#`                    | Available for a bind | Available for a custom bind |

## Limits and storage

The launcher creates a fresh temporary game directory every time it starts and
removes it when TyrQuake exits. Settings, bindings, and saved games are
therefore discarded after each run. The PAK files remain where they were
provided, either on the Nokia card or in INOI tmpfs until the RAM session ends.

Audio is not available. TyrQuake is built with no sound backend.

## Remove

Exit TyrQuake first, then remove its APK if it is no longer needed in the
current session:

```sh
# Source checkout
./fplinux console "$target" --exec 'apk del fplinux-tyrquake'

# Standalone archive
./runner/run.py --reconnect --exec 'apk del fplinux-tyrquake'
```
