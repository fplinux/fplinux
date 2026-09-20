# FPLinux: ARMADA

FPLinux: ARMADA is an optional UMS9117 showcase. Its native software renderer
presents a 3D scene with LCD brightness, keypad light and vibration following
the same timeline where the selected phone supports those physical effects.

The application requires a compatible framebuffer, keypad, LCD backlight,
keypad light and vibrator. See the [target documentation](../../targets/README.md)
for hardware support and physical-effect limitations on the selected phone.

The package is not part of the normal root filesystem and does not start as a
service. In the default RAM boot, installation lasts only for the current
session. On the writable microSD system root, it remains installed until it is
removed. Follow the [microSD system-root shutdown rules](../guides/MICROSD_ROOT.md).

## Install

The APK filename is `fplinux-showcase.apk`; its installed package name is
`fplinux-showcase`. Follow
[Installing and removing optional APK packages](../guides/APK_PACKAGES.md) for
either a source checkout or a standalone archive.

The source-checkout examples below use `inoi-244-modern-4g` with the `default`
profile. Replace both values with the target and profile of the running phone.

## Run

`fplinux-showcase -h` or `fplinux-showcase --help` lists the options without
starting the presentation. Value options accept `--name VALUE` or
`--name=VALUE` and may each appear only once.

Start the complete presentation from the host. The command returns when the
presentation exits:

```sh
# Source checkout
./fplinux console inoi-244-modern-4g --profile default --exec \
  'fplinux-showcase'

# Standalone archive
./runner/run.py --reconnect --exec 'fplinux-showcase'
```

For a bounded presentation run, pass a positive number of complete 38-second
cycles. This is useful when the host console should capture the renderer's
actual result without manually stopping the phone:

```sh
# One complete cycle, then normal cleanup and a result line on the console
./fplinux console inoi-244-modern-4g --profile default --exec \
  'fplinux-showcase --runs 1'
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

The application accepts a two-page RGB565 framebuffer at `240x320` or
`128x160`. The renderer derives its projection and scene layout from the active
framebuffer geometry.

LCD brightness and keypad light use Linux backlight and LED interfaces. The
application discovers them automatically and refuses to start if either is
missing or ambiguous. `--lcd-backlight DIR` and `--keypad-led DIR` select the
respective sysfs device directory explicitly; each directory must provide
`brightness` and `max_brightness`. LCD `max_brightness` must be at least 10.
The keypad must provide the right soft key, and the vibrator must support
`FF_RUMBLE`. Missing required controls prevent the presentation from starting.

The presentation switches the keypad light and vibrator on and off; it varies
their pulse timing, not their intensity. Daylight and LCD brightness change
continuously throughout the complete presentation. During the hardware act the
keypad light and vibrator
transmit `FPLINUX` in Morse code while the corresponding letter pulses on
screen. The final two seconds hold on the water after the letters submerge.
There is no audio output; vibration carries rhythm, not musical pitch.

## Remove

Exit the presentation first, then follow the shared
[package removal instructions](../guides/APK_PACKAGES.md#remove-a-package) for
`fplinux-showcase`.
