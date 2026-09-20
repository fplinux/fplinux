# Keypad backlight

The keypad backlight is exposed as the binary Linux LED-class device
`/sys/class/leds/:kbd_backlight`. The selected
[target's documentation](../../targets/README.md) states whether the physical
light works. A standalone archive carries that status in `README.txt`.

## Interface

A physical key press requests the backlight for about five seconds. The same
bounded light can be requested or cancelled through the standard brightness
attribute:

```sh
echo 1 > /sys/class/leds/:kbd_backlight/brightness
echo 0 > /sys/class/leds/:kbd_backlight/brightness
```

The interface supports only off and on; it does not provide user-selectable
brightness levels. An on request is temporary rather than a persistent lighting
mode.
