# Installable applications

This guide covers APKs published beside a source-checkout build. It does not
apply to a standalone release archive: use that archive's `README.txt` instead.
The applications run only in the current volatile RAM session and are not part
of the standard root filesystem.

## Before installing

Build and start the exact target as described in [Building FPLinux](BUILDING.md)
and its phone document. The build prints an `output:` directory. Set `bundle`
to that directory and `target` to the selected target before using the commands
below:

```sh
target=inoi-244-modern-4g
bundle=/absolute/path/printed-by-fplinux-build
```

Use the target document for its storage, input, screen, and hardware limits.
The commands below only install and remove APKs; a successful install is not a
hardware or release qualification record.

## TyrQuake

`fplinux-tyrquake.apk` contains TyrQuake 0.71, but not Quake game data. Before
starting the game, follow the selected target's instructions for a legally
obtained `pak0.pak` and its storage path.

Install the APK in the running session:

```sh
./fplinux console "$target" --upload \
  "$bundle/apks/fplinux-tyrquake.apk" /tmp/fplinux-tyrquake.apk
./fplinux console "$target" --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-tyrquake.apk'
```

Start one supported input mode:

```sh
./fplinux console "$target" --exec 'quake --input phone'
./fplinux console "$target" --exec 'quake --input keyboard'
```

After the game exits, remove it without restarting the phone:

```sh
./fplinux console "$target" --exec 'apk del fplinux-tyrquake'
```

## MicroPythonOS

All current targets use the base `fplinux-micropythonos.apk`. Install it in the
running session, then attach to the phone shell and run `micropythonos`:

```sh
./fplinux console "$target" --upload \
  "$bundle/apks/fplinux-micropythonos.apk" /tmp/fplinux-micropythonos.apk
./fplinux console "$target" --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk'
./fplinux console "$target"
```

Press `Ctrl-C` at the phone shell to stop MicroPythonOS and restore the terminal.
Then detach with `Ctrl-]` and remove the package:

```sh
./fplinux console "$target" --exec 'apk del fplinux-micropythonos'
```

### Nokia TA-1618 companion

The Nokia TA-1618 additionally requires
`fplinux-micropythonos-nokia-ta1618.apk`. Upload both packages and install them
together instead of using the base-only install and removal commands above:

```sh
target=nokia-ta1618
./fplinux console "$target" --upload \
  "$bundle/apks/fplinux-micropythonos.apk" /tmp/fplinux-micropythonos.apk
./fplinux console "$target" --upload \
  "$bundle/apks/fplinux-micropythonos-nokia-ta1618.apk" \
  /tmp/fplinux-micropythonos-nokia-ta1618.apk
./fplinux console "$target" --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk /tmp/fplinux-micropythonos-nokia-ta1618.apk'
./fplinux console "$target"
./fplinux console "$target" --exec \
  'apk del fplinux-micropythonos-nokia-ta1618 fplinux-micropythonos'
```

The Nokia document describes its card-backed state and the supported storage
lifecycle. Other targets use RAM-only state because their FPLinux targets have
no supported microSD path.
