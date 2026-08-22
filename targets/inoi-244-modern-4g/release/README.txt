FPLinux for INOI 244 Modern 4G

This archive starts an experimental Linux session in volatile RAM. It does not
flash the phone or access its internal storage. The current project provides no
prebuilt release archive or recorded qualified payload for this target. An
archive containing CANDIDATE-NOTICE.txt is a local hardware-qualification
candidate and must not be published as a release.

Qualification binds the exact RAM runtime, loader and host runtime tools, runtime
assets, runner, platform adapter, runtime manifest and bundled top-level
apks/*.apk files.
This README, license notices, provenance and build records, checksums and the
candidate notice are archive metadata outside that phone-qualified payload.
The current source artifact has physical evidence for the inherited-state USB
profile at High-Speed, including shell, verified data pull and physical
reconnect. That evidence does not qualify this archive's exact payload. Feature
statements below cover only the paths that were exercised.

What works on the INOI 244 Modern 4G:
  - local 240x320 terminal and physical keypad;
  - High-Speed USB shell, verified data pull and physical reconnect on
    interface 0;
  - one forwarded host keyboard on interface 1;
  - installable TyrQuake with either input mode;
  - installable MicroPythonOS launcher, navigation and keypad text input.

microSD, internal phone storage, audio, modem, Bluetooth, Wi-Fi and Linux
power-off are not supported by this target.

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

TyrQuake 0.71 is provided as ./apks/fplinux-tyrquake.apk and is not installed
in the standard root filesystem. Install it in the current RAM session:
  ./host/fplinux-usb-console --interface 0 --upload \
    ./apks/fplinux-tyrquake.apk /tmp/fplinux-tyrquake.apk
  ./host/fplinux-usb-console --interface 0 --exec \
    'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-tyrquake.apk'

This phone has no microSD driver, so use tmpfs for a legally obtained pak0.pak:
  ./host/fplinux-usb-console --interface 0 --exec \
    'mkdir -p /mnt/card && mount -t tmpfs none /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
  ./host/fplinux-usb-console --interface 0 --upload \
    ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak
  ./host/fplinux-usb-console --interface 0 --exec 'quake --input phone'
  ./host/fplinux-usb-console --interface 0 --exec 'quake --input keyboard'

The phone has 64 MiB of RAM and the game reserves 32 MiB. A full PAK in tmpfs
leaves little memory; there is no swap. After exiting TyrQuake, remove it with:
  ./host/fplinux-usb-console --interface 0 --exec 'apk del fplinux-tyrquake'

MicroPythonOS is ./apks/fplinux-micropythonos.apk. It targets the shared FPLinux
display and keypad ABI. Installation and removal, the 240x320 launcher, keypad
text input and terminal restoration have been exercised. Individual bundled
applications outside those paths are not qualified. This target has no supported
microSD path, so application state remains in RAM:
  ./host/fplinux-usb-console --interface 0 --upload \
    ./apks/fplinux-micropythonos.apk /tmp/fplinux-micropythonos.apk
  ./host/fplinux-usb-console --interface 0 --exec \
    'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk'
  ./host/fplinux-usb-console --interface 0

At the phone shell prompt, run micropythonos. Press Ctrl-C to stop it and restore
the terminal, then Ctrl-] to detach before removing it with:
  ./host/fplinux-usb-console --interface 0 --exec 'apk del fplinux-micropythonos'

To end the RAM-only session, disconnect USB, remove and reinsert the battery,
then boot the phone normally.
