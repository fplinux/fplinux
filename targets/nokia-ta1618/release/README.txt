FPLinux for Nokia 3210 4G (TA-1618)

This archive starts Linux in volatile RAM. It does not flash the phone or access
its internal storage. The current project provides no prebuilt archive or
qualified runtime closure: this archive is a local hardware qualification
candidate, not a release.

What works on the Nokia 3210 4G (TA-1618):
  - local 240x320 terminal, physical keypad and keypad backlight;
  - USB shell and file transfer on interface 0;
  - one forwarded host keyboard on interface 1;
  - microSD access when the card is inserted before boot;
  - battery-only power-off;
  - TyrQuake with either input mode.

Internal phone storage, audio, modem, Bluetooth, Wi-Fi, battery reporting and
Linux reboot are not supported by this target.

Host requirements:
  - Linux x86-64;
  - Python 3.11 or newer;
  - GNU coreutils (stdbuf);
  - USB permission for 1782:4d00 and 0525:a4a6.

The bundled host tools are static executables. They do not require a particular
host libc, libusb or libudev package.

Start:
  1. Extract the complete top-level directory and enter it.
  2. Check the extracted files: sha256sum -c SHA256SUMS
  3. Power the phone off and disconnect USB.
  4. Start the loader: ./runner/run.py
  5. Wait until the loader explicitly asks for the phone.
  6. Only then hold * and connect the powered-off phone, keeping * held as
     instructed.

Do not connect the phone before starting the loader. If it was connected early,
disconnect it and restart this sequence.

Use after boot:
  - Ctrl-] detaches the host console without stopping Linux.
  - Reconnect to the USB shell with:
      ./host/fplinux-usb-console --interface 0
  - Forward one host keyboard on interface 1 with:
      sudo ./host/fplinux-usb-console --interface 1 --keyboard /dev/input/eventN
    The selected keyboard does not reach the host desktop while forwarding runs.
    On a host with no second keyboard, use:
      sudo timeout 60 ./host/fplinux-usb-console --interface 1 --keyboard /dev/input/eventN
    GNU timeout sends SIGTERM and the client releases the keyboard; keys on the
    grabbed keyboard do not stop the host process.

microSD:
  - Insert the card before Linux starts. Hot-swap is not supported.
  - Mount a FAT card read/write:
      card=/dev/mmcblk0p1
      [ -b "$card" ] || card=/dev/mmcblk0
      mkdir -p /mnt/card
      mount -t vfat -o rw "$card" /mnt/card
  - Before removing the card or ending the session, run:
      sync
      umount /mnt/card

TyrQuake 0.71 is included, but game data is not. Put a legally obtained PAK at:
  /mnt/card/fplinux/quake/id1/pak0.pak
Then start exactly one mode from the phone shell:
  quake --input phone
  quake --input keyboard

The launcher uses temporary runtime storage. TyrQuake settings and saves are
discarded when it exits; they are not written to microSD.

Phone mode uses the physical keypad. Hold the phone counter-clockwise with the
display on the left and keypad on the right:
  - D-pad UP/DOWN: menu left/right; game turn left/right.
  - D-pad LEFT/RIGHT: menu down/up; game backward/forward.
  - Centre or dial: select/fire. Right soft: back/menu.
  - Left soft or *: jump. 0: fire. 1/3: strafe. 2/5: turn.
  - 4/6: backward/forward. 7/9: previous/next weapon. 8: run.

The keypad backlight can be controlled from the phone shell:
  echo 1 > /sys/class/leds/:kbd_backlight/brightness
  echo 0 > /sys/class/leds/:kbd_backlight/brightness
Each physical keypad press also lights it for about five seconds.

To end the RAM session, detach with Ctrl-], disconnect USB, make sure charger
power is absent, then hold the red handset key continuously for five seconds.
Releasing it earlier cancels shutdown. A successful shutdown discards the RAM
session; boot normally to return to the vendor firmware. If the phone remains
powered after shutdown starts, remove and reinsert the battery before booting.
Linux reboot is not supported.
