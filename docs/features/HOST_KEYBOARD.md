# Host keyboard bridge

The keyboard bridge forwards one Linux evdev keyboard from the host to the
phone through FPLinux's USB gadget. It becomes a normal virtual keyboard on the
phone, so it works with the [local console](LOCAL_CONSOLE.md) and software that
accepts regular Linux keyboard input.

The selected host device is grabbed for the whole forwarding session. Its keys
will not reach the host desktop, terminal or the program that started the
bridge. Use a second keyboard where possible; otherwise put a timeout around
the command so the grab is released automatically.

## Start forwarding

```sh
# Source checkout
sudo timeout 60s ./fplinux console <target> --keyboard /dev/input/eventN

# Standalone archive
sudo timeout 60s ./host/fplinux-usb-keyboard \
  --interface 0 --keyboard /dev/input/eventN
```

`/dev/input/eventN` must be the evdev node of the keyboard to forward. Elevated
privileges are normally needed to open that input node and the USB interface.
The source-checkout command gets the gadget identity and interface from the
selected target; the current standalone archives use generic-serial interface
`0`.

The bridge can run alongside an SSH shell, one-off command, upload or download.
If the USB connection or keyboard disappears, the phone releases forwarded keys
instead of leaving a modifier held. Stopping the client also releases the host
keyboard grab.

[TyrQuake](../apps/TYRQUAKE.md) accepts the forwarded device in its keyboard
input mode. [MicroPythonOS](../apps/MICROPYTHONOS.md) reads the normalized phone
keypad instead and does not accept the host keyboard bridge.

This feature forwards a host keyboard over the phone's USB peripheral link; it
does not make the phone a USB host. Check the selected phone page for its
support status.
