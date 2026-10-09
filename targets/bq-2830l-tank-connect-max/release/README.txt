FPLinux for BQ 2830L Tank Connect Max

Use an archive for this exact phone. On a Linux x86-64 PC, install Python 3.14,
GNU coreutils, iproute2 and OpenSSH. The PC must configure a new USB network
interface through IPv4 DHCP. Use a USB data cable.

From the extracted archive directory, check its files and install USB rules:

  sha256sum -c SHA256SUMS
  sudo install -m 0644 ./60-fplinux.rules /etc/udev/rules.d/60-fplinux.rules
  sudo udevadm control --reload-rules

Power the phone off and disconnect USB. Start the runner as your regular user:

  ./runner/run.py

Wait until the runner asks for the powered-off phone. Then hold the green call
key and connect USB, keeping the key pressed. If connected too early, disconnect
and restart this sequence. Exit the shell to return to the PC; Linux keeps
running. Reconnect with:

  ./runner/run.py --reconnect

Shared archive procedures:
https://fplinux.github.io/fplinux/guides/standalone-archive/

This target starts Linux in RAM with a 240x320 display and a USB console.
Prepare device data from this same handset for fitted audio and radio data.
Physical keypad controls, speaker output and wireless connectivity are
unqualified. Bluetooth and FM radio are not available.

Phone support and bring-up requirements:
https://fplinux.github.io/fplinux/porting/checklist/

Insert microSD before boot and leave it installed while the phone is powered.
FAT32 and ext4 data cards support reads and writes; a microSD system root is
not supported. Before ending the RAM session, stop applications using the card,
disable card-backed swap, flush writes and unmount card filesystems. Disconnect
USB. If a battery is installed, remove and reinsert it before booting normally.
A fresh USB load is required after power-off.

Shutdown safety:
https://fplinux.github.io/fplinux/use/power/
