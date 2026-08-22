FPLinux for Nokia 3210 4G (TA-1618)

This archive starts Linux in volatile RAM. It does not flash the phone or access
its internal storage. The current project provides no prebuilt release archive
or recorded qualified payload for this target. An archive containing
CANDIDATE-NOTICE.txt is a local hardware-qualification candidate and must not be
published as a release.

Qualification binds the exact RAM runtime, loader and host runtime tools, runtime
assets, runner, platform adapter, runtime manifest and bundled top-level
apks/*.apk files.
This README, license notices, provenance and build records, checksums and the
candidate notice are archive metadata outside that phone-qualified payload.
The current source artifact has physical evidence for cold USB initialization,
High-Speed `g_serial`, shell/data pull and a physical USB reconnect. That
evidence does not qualify this archive's exact payload. USB upload,
host-keyboard forwarding and application procedures below remain candidate
procedures, not physical qualification of this archive.

What works on the Nokia 3210 4G (TA-1618):
  - local 240x320 terminal, physical keypad and keypad backlight;
  - microSD access when the card is inserted before boot;
  - battery-only power-off;

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

Candidate USB application procedures:

Bundled APKs under ./apks are not installed in the standard root filesystem.
The commands below are unqualified candidate procedures. They install and
remove each application in the current RAM session without restarting the phone.

TyrQuake 0.71 is fplinux-tyrquake.apk; game data is not included:
  ./host/fplinux-usb-console --interface 0 --upload \
    ./apks/fplinux-tyrquake.apk /tmp/fplinux-tyrquake.apk
  ./host/fplinux-usb-console --interface 0 --exec \
    'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-tyrquake.apk'

Put a legally obtained PAK at:
  /mnt/card/fplinux/quake/id1/pak0.pak
Then start exactly one mode:
  ./host/fplinux-usb-console --interface 0 --exec 'quake --input phone'
  ./host/fplinux-usb-console --interface 0 --exec 'quake --input keyboard'

After exiting TyrQuake, remove it with:
  ./host/fplinux-usb-console --interface 0 --exec 'apk del fplinux-tyrquake'

MicroPythonOS requires its base and TA-1618 companion packages:
  ./host/fplinux-usb-console --interface 0 --upload \
    ./apks/fplinux-micropythonos.apk /tmp/fplinux-micropythonos.apk
  ./host/fplinux-usb-console --interface 0 --upload \
    ./apks/fplinux-micropythonos-nokia-ta1618.apk \
    /tmp/fplinux-micropythonos-nokia-ta1618.apk
  ./host/fplinux-usb-console --interface 0 --exec \
    'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk /tmp/fplinux-micropythonos-nokia-ta1618.apk'
  ./host/fplinux-usb-console --interface 0

At the phone shell prompt, run micropythonos. Press Ctrl-C to stop it and restore
the terminal, then Ctrl-] to detach before removing it with:
  ./host/fplinux-usb-console --interface 0 --exec \
    'apk del fplinux-micropythonos-nokia-ta1618 fplinux-micropythonos'

With a usable FAT32 microSD card inserted before boot, the candidate
MicroPythonOS procedure uses `/mnt/card` and stores its state under
`/mnt/card/.fplinux/micropythonos`. It mounts the card when no matching mount
exists and unmounts only a card that it mounted when the interface exits.
Otherwise its state remains in RAM. This procedure, including installation and
removal, the launcher, keypad text input, File Manager and card-backed state,
is hardware-unqualified for the current cold-owned USB profile. Individual
bundled applications outside those paths are also unqualified.

The TyrQuake launcher uses temporary runtime storage. Its settings and saves
are discarded when it exits; they are not written to microSD.

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
