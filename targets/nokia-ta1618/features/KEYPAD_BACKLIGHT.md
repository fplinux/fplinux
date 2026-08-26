# Keypad backlight on Nokia 3210 4G (TA-1618)

This page applies only to Nokia 3210 4G (TA-1618). The keypad backlight is
exposed as a binary Linux LED-class device.

## Interface

A physical key press turns the backlight on for about five seconds. The same
bounded light can be requested or cancelled through the standard brightness
attribute:

```sh
echo 1 > /sys/class/leds/:kbd_backlight/brightness
echo 0 > /sys/class/leds/:kbd_backlight/brightness
```

The interface supports only off and on; it does not provide user-selectable
brightness levels. An on request is temporary rather than a persistent lighting
mode.
