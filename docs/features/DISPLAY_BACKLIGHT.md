# LCD backlight

The system brightness control uses the same `0` through `10` scale on phones
with a configured LCD backlight. Run `fplinux-brightness get` to read the
selected level, or `fplinux-brightness set LEVEL` to change it. Level `0`
switches the backlight off; level `7` is selected on a fresh boot. The setting
survives a restart of the brightness service during the same boot, but is not
saved across phone boots.

The brightness service translates each level to the selected phone's current
code. An application can temporarily change the visible level while it owns
the display. `get` continues to report the user's selected level, and a change
made with `set` during the temporary effect is applied when the application
releases it. The levels are not percentages or calibrated optical units;
equal numbers on different phones do not establish equal visible brightness.

The selected [target's documentation](../../targets/README.md) states its
physical support and raw hardware range. A standalone archive carries that
status in `README.txt`.

## Raw hardware interface

The standard Linux backlight class remains available under
`/sys/class/backlight/` for diagnostics. Read `brightness`,
`actual_brightness` and `max_brightness` from the selected backlight device.
Writing an integer from `0` through `max_brightness` directly to `brightness`
bypasses the system's `0…10` mapping; a later managed change may replace it.
Raw level `0` switches the WLED off.
`actual_brightness` reports the applied driver level, not a measurement of
visible light.

The raw values are board-specific current steps. Their range and physical
effect differ by phone; an available interface does not demonstrate that its
levels visibly change the screen. FPLinux does not provide automatic brightness
control.

## Power and display lifecycle

The standard `bl_power` attribute accepts `0` for on and `4` for off. Turning
the backlight off does not stop scanout or put the panel to sleep.

DRM display disable and s2idle remain authoritative for the complete display
lifecycle. While the display is disabled, a brightness write updates the
requested value without lighting the screen. Display enable or wake applies that value
only after the first completed frame. Display errors, shutdown and driver
removal leave the WLED off.
