# FPLinux: ARMADA

FPLinux: ARMADA is an optional Nokia 3210 4G (TA-1618) showcase. Its native
software renderer presents a synchronized 3D scene while the LCD brightness,
keypad light and vibrator reinforce the same timeline.

The package is not part of the normal root filesystem and does not start as a
service. In the default RAM boot, installation lasts only for the current
session. On the writable microSD system root, it remains installed until it is
removed. Follow the microSD shutdown rules in
`targets/nokia-ta1618/profiles/microsd-uboot/features/MICROSD.md` from a source
checkout or `docs/target/MICROSD.md` from the standalone archive.

## Install

### Source checkout

Set `bundle` to the `output:` directory printed by the Nokia build, then upload
and install the bundled APK:

```sh
target=nokia-ta1618
bundle=/absolute/path/printed-by-fplinux-build

./fplinux console "$target" --upload \
  "$bundle/apks/fplinux-showcase.apk" /tmp/fplinux-showcase.apk
./fplinux console "$target" --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-showcase.apk'
```

### Standalone archive

From the extracted Nokia archive directory:

```sh
./runner/run.py --reconnect --upload \
  ./apks/fplinux-showcase.apk /tmp/fplinux-showcase.apk
./runner/run.py --reconnect --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-showcase.apk'
```

## Run

Start the complete presentation from the host. The command returns when the
presentation exits:

```sh
# Source checkout
./fplinux console nokia-ta1618 --exec 'fplinux-showcase'

# Standalone archive
./runner/run.py --reconnect --exec 'fplinux-showcase'
```

For a bounded presentation run, pass a positive number of complete 38-second
cycles. This is useful when the host console should capture the renderer's
actual result without manually stopping the phone:

```sh
# One complete cycle, then normal cleanup and a result line on the console
./fplinux console nokia-ta1618 --exec 'fplinux-showcase --runs 1'
```

After a normal exit, the application prints one result line to stdout:

```text
runs=1 rendered=... average_fps=X.Y minimum_fps=X.Y average_frame_ms=... maximum_frame_ms=... cpu_percent=X.Y peak_rss_kib=...
```

`runs` is the number of complete timeline cycles. In the default unbounded
mode, or if a bounded run is stopped early, it reports the number of cycles
that had completed when the application exited. `rendered` is the number of
frames actually presented; skipped timeline frames are not counted. Frame
times cover rendering, copying and presenting a frame, while `average_fps`
covers the complete presentation interval. `cpu_percent` is this process's
average CPU use over the run; `peak_rss_kib` is its peak resident memory as
reported by Linux. Neither value describes total system memory.

Press the phone's right soft key to exit at any time. The application then
returns the display to the local console, restores the LCD brightness it found
at startup, restores the keypad-light level it found at startup, switches off
its vibration request, and releases the input and framebuffer devices. The same
cleanup runs after a handled termination signal or startup failure.

## Display and physical effects

The current package supports the Nokia's `240x320` RGB565 framebuffer. The
renderer derives its projection and scene layout from the active framebuffer
geometry, but no `128x160` phone is currently supported by this package.

LCD brightness uses the Nokia backlight interface. The keypad light and
vibrator are binary devices: the presentation can vary their pulse timing, not
their intensity. Daylight and LCD brightness change continuously throughout the
complete presentation. During the hardware act the keypad light and vibrator
transmit `FPLINUX` in Morse code while the corresponding letter pulses on
screen. The final two seconds hold on the water after the letters submerge.
There is no audio output; vibration carries rhythm, not musical pitch.

## Remove

Exit the presentation first, then remove its package from the active system
root:

```sh
# Source checkout
./fplinux console nokia-ta1618 --exec 'apk del fplinux-showcase'

# Standalone archive
./runner/run.py --reconnect --exec 'apk del fplinux-showcase'
```
