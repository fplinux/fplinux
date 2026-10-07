FPLinux for Maxvi K15n4G

Use an archive for this exact phone. On a Linux x86-64 PC, install Python 3.14,
GNU coreutils, iproute2 and OpenSSH. The PC must configure a new USB network
interface through IPv4 DHCP. Use a USB data cable.

From the extracted archive directory, check its files and install USB rules:

  sha256sum -c SHA256SUMS
  sudo install -m 0644 ./60-fplinux.rules /etc/udev/rules.d/60-fplinux.rules
  sudo udevadm control --reload-rules

Power the phone off and disconnect USB. Start the runner as your regular user:

  ./runner/run.py

Wait until the runner asks for the powered-off phone. Then hold the left soft key
and connect USB, keeping the key pressed. If connected too early, disconnect
and restart this sequence. Exit the shell to return to the PC; Linux keeps
running. Reconnect with:

  ./runner/run.py --reconnect

Shared archive procedures:
https://fplinux.github.io/fplinux/guides/standalone-archive/

This target starts Linux with a volatile RAM root. A fresh USB load is required
after power-off.

Phone support, feature procedures and limitations:
https://fplinux.github.io/fplinux/phones/maxvi-k15n-4g/

Before ending a RAM session, stop programs using microSD, disable card-backed
swap, run sync and unmount every card filesystem. Only then disconnect USB.
If a battery is installed, remove and reinsert it before booting normally.

Storage and shutdown safety:
https://fplinux.github.io/fplinux/use/microsd/
https://fplinux.github.io/fplinux/use/power/
