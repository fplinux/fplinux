# TyrQuake

TyrQuake 0.71 is an optional native game for FPLinux.
The APK contains the engine, not Quake game data. Provide a legally obtained
`pak0.pak` before starting the game. First load or reconnect to the selected
phone using its instructions.
A standalone archive includes this page; start with its top-level `README.txt`.

## Install

The package is installed separately into the active system root. Installation
lasts until shutdown in the `default` RAM profile and persists across boots in
`microsd-uboot`.

### Source checkout

Set `target` and `profile` to match the running system and `bundle` to the
`output:` directory from its build, then upload and install the APK:

```sh
target=inoi-244-modern-4g
profile=default
bundle=/absolute/path/printed-by-fplinux-build

./fplinux console "$target" --profile "$profile" --upload \
  "$bundle/apks/fplinux-tyrquake.apk" /tmp/fplinux-tyrquake.apk
./fplinux console "$target" --profile "$profile" --exec \
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

### Data card in the RAM profile

Use a filesystem supported by the selected phone: FAT32 on Nokia TA-1618,
or ext4 on either INOI target. Follow the shared
[microSD instructions](../features/MICROSD.md) to mount it at `/mnt/card`,
make `fplinux/quake/id1`, and upload the PAK using
[file transfer](../features/FILE_TRANSFER.md). Follow the shared safe-removal
procedure before removing the card.

On a supported FAT32 data card, `quake` can mount the card read-only when
`/mnt/card` is not already mounted. Mount ext4 manually before launching the
game. Game data remains on the card, but the game never writes saves or
settings there.

### microSD system root

With `microsd-uboot`, the system ext4 root is not automatically made available
at `/mnt/card`. Keep game data in a directory on the root filesystem and bind
mount that directory at `/mnt/card` before launching the game. For example,
with `/mnt/card` not already mounted, run on the phone:

```sh
mkdir -p /var/lib/quake-data/fplinux/quake/id1 /mnt/card
mount -o bind /var/lib/quake-data /mnt/card
```

Upload the PAK to `/mnt/card/fplinux/quake/id1/pak0.pak`. The data persists on
the ext4 root; repeat the bind mount after each boot. Keep the `FPLBOOT`
partition reserved for boot files.
Follow [microSD root shutdown](../guides/MICROSD_ROOT.md#persistence-and-shutdown)
before removing or rewriting the system card.

### Without a data card

In the RAM profile, when `/mnt/card` is not already mounted, create a temporary
mount and copy the PAK into it for this session:

```sh
# Source checkout
./fplinux console "$target" --profile "$profile" --exec \
  'mkdir -p /mnt/card && mount -t tmpfs tmpfs /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
./fplinux console "$target" --profile "$profile" --upload \
  ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak

# Standalone archive
./runner/run.py --reconnect --exec \
  'mkdir -p /mnt/card && mount -t tmpfs tmpfs /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
./runner/run.py --reconnect --upload \
  ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak
```

The PAK consumes phone RAM and disappears with the session. A full PAK leaves
less room for the game.

## Run

Choose one input mode. `phone` uses the physical keypad. `keyboard` uses the
host keyboard forwarded through the USB keyboard bridge; it does not combine
with the phone keypad. Start forwarding first as described in
[Host keyboard forwarding](../features/HOST_KEYBOARD.md).

```sh
# Source checkout
./fplinux console "$target" --profile "$profile" --exec 'quake --input phone'
./fplinux console "$target" --profile "$profile" --exec 'quake --input keyboard'

# Standalone archive
./runner/run.py --reconnect --exec 'quake --input phone'
./runner/run.py --reconnect --exec 'quake --input keyboard'
```

The command keeps the phone display in game mode until TyrQuake exits.

The launcher requires exactly one `--input` option. A duplicate or an extra
argument is rejected before game data is mounted or TyrQuake starts.

`--heapsize` sets the memory TyrQuake reserves for itself, in kibibytes,
between 8192 and 262144; the default is 32768. The reservation is separate from
the game data, so a phone that holds that data in RAM has correspondingly less
room and may need a smaller figure:

```sh
./fplinux console "$target" --profile "$profile" --exec \
  'quake --input phone --heapsize 16384'
```

A size below what the selected game data needs makes TyrQuake stop during
startup rather than run with less.

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
therefore discarded after each run in both profiles. The PAK files remain
where they were provided: files on persistent storage survive a boot, while
files in tmpfs disappear when the system shuts down.

Audio is not available. TyrQuake is built with no sound backend.

## Remove

Exit TyrQuake first, then remove its APK from the active system root if it is
no longer needed:

```sh
# Source checkout
./fplinux console "$target" --profile "$profile" --exec 'apk del fplinux-tyrquake'

# Standalone archive
./runner/run.py --reconnect --exec 'apk del fplinux-tyrquake'
```
