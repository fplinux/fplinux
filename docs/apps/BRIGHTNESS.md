# Brightness control

`fplinux-brightness-ui` is an optional phone application for the shared LCD
brightness scale. It shows the selected level from `0` through `10` and a
ten-segment indicator. The number is a setting, not a percentage or a light
measurement. The [LCD backlight guide](../features/DISPLAY_BACKLIGHT.md)
explains the system control and each target's raw hardware range.

The application needs the brightness service, a compatible RGB565 DRM display
and the phone keypad. It supports the current `128×160` and `240×320` display
sizes. The selected [target](../../targets/README.md) states the physical
brightness limits for that phone.

## Install

The bundled APK is `fplinux-brightness-ui.apk`; its installed package name is
`fplinux-brightness-ui`. Follow the shared
[APK installation procedure](../guides/APK_PACKAGES.md). The application does
not start automatically.

## Run and control

Start it from the phone's local terminal with `fplinux-brightness-ui`. From a
source checkout, run it on the already booted phone (replace the target name
for another phone):

```sh
./fplinux console nokia-ta1618 --exec 'fplinux-brightness-ui'
```

From an extracted standalone archive that started the running phone:

```sh
./runner/run.py --reconnect --exec 'fplinux-brightness-ui'
```

Press **Up** to increase one level, **Down** to decrease one level, and the
**right soft key** to exit. The range stops at `0` and `10`. Level `0` turns
the LCD backlight off; press **Up** to restore level `1` even though the screen
is dark. A successful change becomes the system's selected level and remains
in effect after the application exits. A failed change keeps the last
confirmed level and reports the error on screen when the backlight is lit.

The setting is retained during this boot, including across a brightness
service restart, but is not saved across phone boots. The application uses no
audio or vibration.
