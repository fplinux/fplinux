# Keypad backlight

The keypad backlight is exposed as the binary Linux LED-class device
`/sys/class/leds/:kbd_backlight`. The selected
[target's documentation](../../targets/README.md) states whether the physical
light works. A standalone archive carries that status in `README.txt`.

## Interface

The default `input-events` trigger turns the backlight on during input activity
and off about five seconds after the last event. It includes local keys and
forwarded keyboard input.

To control the light manually, select the `none` trigger, then use brightness:

```sh
echo none > /sys/class/leds/:kbd_backlight/trigger
echo 1 > /sys/class/leds/:kbd_backlight/brightness
echo 0 > /sys/class/leds/:kbd_backlight/brightness
```

The interface supports only off and on; it does not provide user-selectable
brightness levels. Manual lighting remains on until it is switched off. Restore
automatic lighting with:

```sh
echo input-events > /sys/class/leds/:kbd_backlight/trigger
```
