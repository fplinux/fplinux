# LCD backlight on Nokia 3210 4G (TA-1618)

The LCD backlight is exposed through the standard Linux backlight class:

```text
/sys/class/backlight/ta1618-backlight
```

## Brightness

Read or select one of the eleven supported levels:

```sh
cat /sys/class/backlight/ta1618-backlight/brightness
cat /sys/class/backlight/ta1618-backlight/actual_brightness
echo 5 > /sys/class/backlight/ta1618-backlight/brightness
```

`brightness` is the requested level from `0` through `10`.
`actual_brightness` reports the level currently applied to the hardware.
Level `0` switches the WLED off; levels `1` through `10` increase monotonically,
and `10` is the default and maximum supported level.

The levels are raw board-qualified current steps, not percentages or calibrated
optical units. FPLinux does not provide automatic brightness control.

## Power and display lifecycle

The standard `bl_power` attribute accepts `0` for on and `4` for off. Turning
the backlight off does not stop the framebuffer or put the panel to sleep.

Framebuffer blanking and s2idle remain authoritative for the complete display
lifecycle. While the framebuffer is blank, a brightness write updates the
requested value without lighting the screen. Unblank or wake applies that value
only after the first completed frame. Display errors, shutdown and driver
removal leave the WLED off.
