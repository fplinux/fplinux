FPLinux for UNIWA V1000 4G

Use an archive for this exact phone. On a Linux x86-64 PC, install Python 3.14,
GNU coreutils, iproute2 and OpenSSH. The PC must configure a new USB network
interface through IPv4 DHCP. Use a USB data cable.

From the extracted archive directory, check its files and install USB rules:

  sha256sum -c SHA256SUMS
  sudo install -m 0644 ./60-fplinux.rules /etc/udev/rules.d/60-fplinux.rules
  sudo udevadm control --reload-rules

Power the phone off and disconnect USB. Start the runner as your regular user:

  ./runner/run.py

Wait until the runner asks for the powered-off phone. Then hold D-pad Up
and connect USB, keeping the key pressed. If connected too early, disconnect
and restart this sequence. Exit the shell to return to the PC; Linux keeps
running. Reconnect with:

  ./runner/run.py --reconnect

Shared archive procedures:
https://fplinux.github.io/fplinux/guides/standalone-archive/

This target starts Linux in RAM with a readable 320x240 landscape terminal.
The runner reports loading progress on the PC. Use the USB SSH session for
command input; full keypad operation is unqualified.

Command-line brightness control works at logical levels 0, 5 and 10 in RAM.
The brightness application displays and changes the level; physical keypad
control and brightness calibration are unqualified.

A microSD system card also provides a readable terminal and native 320x240
TyrQuake demo playback. Physical game controls and full gameplay are
unqualified. There is no game sound.

Stereo playback works with the tested three-contact headphones and a microSD
system card. The tested four-contact headset plays each channel through both
earpieces. Headset recording and other accessories are unqualified.

The built-in speaker plays audible tones with a microSD system card. Speech
and music quality are unqualified.

Phone support and limits:
https://fplinux.github.io/fplinux/phones/uniwa-v1000-4g/

To end the RAM session, disconnect USB. If a battery is installed, remove and
reinsert it before booting normally. A fresh USB load is required after
power-off.

Shutdown safety:
https://fplinux.github.io/fplinux/use/power/
