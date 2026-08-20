FPLinux for INOI 240 Modern 4G

This archive contains an experimental volatile-RAM Linux payload and its fixed
host runner. It does not flash, erase or access the phone's internal storage.

The console profile provides a local 128x160 framebuffer console, the physical
keypad and two Linux USB serial interfaces. Interface 0 carries the BusyBox shell
and file-transfer modes; interface 1 carries forwarded host-keyboard events into
a persistent uinput keyboard. Audio, modem, Bluetooth, camera, microSD and phone
power-off are not supported.
To leave the RAM-only session, disconnect USB, remove and reinsert the battery,
then boot the phone normally.

The RAM image includes the TyrQuake 0.71 engine but no Quake game data. The
shared backend derives the mode from fbdev: on this panel it renders 320x256,
downsamples by two to 160x128 and rotates into the 128x160 display. TyrQuake
works on physical INOI 240 hardware with both the phone keypad and forwarded host
keyboard. Hardware support does not by itself qualify any archive or runtime
closure as a release. A RAM-only game session uses an already mounted /mnt/card,
for example:

  ./host/fplinux-usb-console --interface 0 --exec \
    'mkdir -p /mnt/card && mount -t tmpfs none /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
  ./host/fplinux-usb-console --interface 0 --upload \
    ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak
  ./host/fplinux-usb-console --interface 0 --exec 'quake --input phone'
  ./host/fplinux-usb-console --interface 0 --exec 'quake --input keyboard'

Forward a host keyboard on interface 1 with:
  sudo ./host/fplinux-usb-console --interface 1 --keyboard /dev/input/eventN
Interface 0 remains available for the shell and file transfers while the keyboard
forwarder owns interface 1.

Host requirements:
  - Linux x86-64
  - Python 3.11 or newer
  - GNU coreutils (stdbuf)
  - USB permissions for devices 1782:4d00 and 0525:a4a6

The bundled native host tools are static executables and do not require host
libusb, libudev or a particular libc implementation.

Start:
  1. Extract the complete top-level directory.
  2. Enter that extracted directory.
  3. Run: sha256sum -c SHA256SUMS
  4. Power the phone off and disconnect USB.
  5. Run: ./runner/run.py
  6. Hold * and connect USB while keeping * pressed when prompted.

Ctrl-] detaches the host console without rebooting the phone. Reconnect while
Linux is still running with ./host/fplinux-usb-console --interface 0. To return
to the stock firmware, disconnect USB and remove and reinsert the battery.
