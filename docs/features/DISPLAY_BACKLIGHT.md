# LCD backlight

The LCD backlight is exposed through the standard Linux backlight class under
`/sys/class/backlight/`. The selected
[target's documentation](../../targets/README.md) states its device name,
brightness range and physical support. A standalone archive carries that status
in `README.txt`.

## Brightness

Read `brightness`, `actual_brightness` and `max_brightness` from the selected
backlight device. Write an integer from `0` through `max_brightness` to
`brightness` to request a level. Level `0` switches the WLED off.
`actual_brightness` reports the applied driver level, not a measurement of
visible light.

The levels are raw board-specific current steps, not percentages or calibrated
optical units. The range and physical effect differ by phone; an available
brightness interface does not establish that its levels are physically
qualified. FPLinux does not provide automatic brightness control.

## Power and display lifecycle

The standard `bl_power` attribute accepts `0` for on and `4` for off. Turning
the backlight off does not stop the framebuffer or put the panel to sleep.

Framebuffer blanking and s2idle remain authoritative for the complete display
lifecycle. While the framebuffer is blank, a brightness write updates the
requested value without lighting the screen. Unblank or wake applies that value
only after the first completed frame. Display errors, shutdown and driver
removal leave the WLED off.
