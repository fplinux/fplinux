FPLinux for INOI 244 Modern 4G

This archive contains an experimental volatile-RAM Linux payload and its fixed
host runner. It does not flash, erase or access the phone's internal storage.

This target intentionally provides only a headless BusyBox shell over Linux USB
serial. The LCD, keypad, audio, modem, Bluetooth, camera, microSD and power-off
paths are not supported. To leave the RAM-only session, disconnect USB, remove
and reinsert the battery, then boot the phone normally.

Host requirements:
  - Linux x86-64 with glibc 2.38 or newer
  - Python 3.11 or newer
  - libusb 1.0, libudev and GNU coreutils (stdbuf)
  - USB permissions for devices 1782:4d00 and 0525:a4a6

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
