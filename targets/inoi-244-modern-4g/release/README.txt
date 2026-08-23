FPLinux for INOI 244 Modern 4G

This candidate archive starts experimental Linux in volatile RAM. It does not
flash the phone or access internal storage, and it is not a release archive.

Current target support:
  - local 240x320 terminal and physical keypad;
  - USB SSH/SFTP and host-keyboard forwarding;
  - installable TyrQuake;
  - installable MicroPythonOS launcher, navigation and keypad text input.

microSD, internal phone storage, audio, modem, Bluetooth, Wi-Fi and Linux
power-off are not supported by this target.

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
      ssh -F "$XDG_RUNTIME_DIR/fplinux/current/inoi-244-modern-4g.ssh-config" fplinux
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

Candidate application procedures:

TyrQuake 0.71 is provided as ./apks/fplinux-tyrquake.apk and is not installed
in the standard root filesystem. Install it in the current RAM session:
  ./runner/run.py --reconnect --upload \
    ./apks/fplinux-tyrquake.apk /tmp/fplinux-tyrquake.apk
  ./runner/run.py --reconnect --exec \
    'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-tyrquake.apk'

This phone has no supported microSD path, so use tmpfs for a legally obtained
pak0.pak:
  ./runner/run.py --reconnect --exec \
    'mkdir -p /mnt/card && mount -t tmpfs none /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
  ./runner/run.py --reconnect --upload \
    ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak
  ./runner/run.py --reconnect --exec 'quake --input phone'
  ./runner/run.py --reconnect --exec 'quake --input keyboard'

The phone has 64 MiB of RAM and the game reserves 32 MiB. A full PAK in tmpfs
leaves little memory; there is no swap. After exiting TyrQuake, remove it with:
  ./runner/run.py --reconnect --exec 'apk del fplinux-tyrquake'

MicroPythonOS is ./apks/fplinux-micropythonos.apk. This target has no supported
microSD path, so application state remains in RAM:
  ./runner/run.py --reconnect --upload \
    ./apks/fplinux-micropythonos.apk /tmp/fplinux-micropythonos.apk
  ./runner/run.py --reconnect --exec \
    'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk'
  ./runner/run.py --reconnect

At the phone shell prompt, run micropythonos. Press Ctrl-C to stop it and restore
the terminal, then exit SSH before removing it with:
  ./runner/run.py --reconnect --exec 'apk del fplinux-micropythonos'

To end the RAM-only session, disconnect USB, remove and reinsert the battery,
then boot the phone normally.
