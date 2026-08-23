FPLinux for Nokia 3210 4G (TA-1618)

This candidate archive starts Linux in volatile RAM. It does not flash the phone
or access internal storage, and it is not a release archive.

Current target support:
  - local 240x320 terminal, physical keypad and keypad backlight;
  - USB SSH/SFTP and host-keyboard forwarding;
  - microSD access when the card is inserted before boot;
  - battery-only power-off;

Internal phone storage, audio, modem, Bluetooth, Wi-Fi, battery reporting and
Linux reboot are not supported by this target.

Host requirements:
  - Linux x86-64;
  - Python 3.11 or newer;
  - GNU coreutils (stdbuf);
  - iproute2 (`ip`);
  - a host network manager that automatically runs IPv4 DHCP on a new USB-NCM
    interface (NetworkManager is supported);
  - OpenSSH client tools (`ssh`, `ssh-keygen`, `ssh-keyscan`, `sftp`);
  - USB permission for 1782:4d00 and 0525:a4a6.

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
  - Exit the SSH shell normally, or use the OpenSSH escape `~.` at the start of
    a line to disconnect without stopping Linux.
  - Reconnect to the SSH shell with:
      ./runner/run.py --reconnect
  - Use ordinary OpenSSH for the same ready session with:
      ssh -F "$XDG_RUNTIME_DIR/fplinux/current/nokia-ta1618.ssh-config" fplinux
    This config is available only while the RAM session is ready.
  - Run a command with:
      ./runner/run.py --reconnect --exec 'uname -r'
  - Upload or pull a file with:
      ./runner/run.py --reconnect --upload ./local.bin /tmp/remote.bin
      ./runner/run.py --reconnect --pull /tmp/remote.bin ./local.bin
  - Forward one host keyboard on generic-serial interface 0 with:
      sudo ./host/fplinux-usb-keyboard --interface 0 --keyboard /dev/input/eventN
    The selected keyboard does not reach the host desktop while forwarding runs.
    On a host with no second keyboard, use:
      sudo timeout 60 ./host/fplinux-usb-keyboard --interface 0 --keyboard /dev/input/eventN
    The client releases the keyboard when the timeout expires.

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

Candidate SSH application procedures:

Bundled APKs under ./apks are not installed in the standard root filesystem.
The commands below install and remove each application in the current RAM
session without restarting the phone.

TyrQuake 0.71 is fplinux-tyrquake.apk; game data is not included:
  ./runner/run.py --reconnect --upload \
    ./apks/fplinux-tyrquake.apk /tmp/fplinux-tyrquake.apk
  ./runner/run.py --reconnect --exec \
    'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-tyrquake.apk'

Put a legally obtained PAK at:
  /mnt/card/fplinux/quake/id1/pak0.pak
Then start exactly one mode:
  ./runner/run.py --reconnect --exec 'quake --input phone'
  ./runner/run.py --reconnect --exec 'quake --input keyboard'

After exiting TyrQuake, remove it with:
  ./runner/run.py --reconnect --exec 'apk del fplinux-tyrquake'

MicroPythonOS requires its base and TA-1618 companion packages:
  ./runner/run.py --reconnect --upload \
    ./apks/fplinux-micropythonos.apk /tmp/fplinux-micropythonos.apk
  ./runner/run.py --reconnect --upload \
    ./apks/fplinux-micropythonos-nokia-ta1618.apk \
    /tmp/fplinux-micropythonos-nokia-ta1618.apk
  ./runner/run.py --reconnect --exec \
    'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk /tmp/fplinux-micropythonos-nokia-ta1618.apk'
  ./runner/run.py --reconnect

At the phone shell prompt, run micropythonos. Press Ctrl-C to stop it and restore
the terminal, then exit SSH before removing it with:
  ./runner/run.py --reconnect --exec \
    'apk del fplinux-micropythonos-nokia-ta1618 fplinux-micropythonos'

With a usable FAT32 microSD card inserted before boot, the candidate
MicroPythonOS procedure uses `/mnt/card` and stores its state under
`/mnt/card/.fplinux/micropythonos`. It mounts the card when no matching mount
exists and unmounts only a card that it mounted when MicroPythonOS exits.
Otherwise its state remains in RAM.

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

To end the RAM session, exit SSH, disconnect USB, make sure charger power is
absent, then hold the red handset key continuously for five seconds.
Releasing it earlier cancels shutdown. A successful shutdown discards the RAM
session; boot normally to return to the vendor firmware. If the phone remains
powered after shutdown starts, remove and reinsert the battery before booting.
Linux reboot is not supported.
