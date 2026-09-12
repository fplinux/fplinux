FPLinux for INOI 240 Modern 4G

Read docs/guides/STANDALONE.md before connecting the phone. It contains the
host requirements, USB setup, checksum, loader-first and reconnect procedures.
This target's BootROM key is * (asterisk); hold it when the loader asks for the
powered-off phone.

Current target support:
  - 128x160 terminal interface;
  - USB SSH/SFTP and host-keyboard forwarding;
  - ARMADA, TyrQuake, image rotation, JPEG codec/scaling and presentation;
  - microSD ext4 read/write, card-backed swap and a writable microSD system root;
  - Bluetooth pairing, bidirectional file transfer and PAN Internet in both
    the default RAM and microsd-uboot profiles;
  - RTC time and alarms, repeated RTC-woken s2idle and peripheral recovery in
    both profiles, including mounted ext4 storage and card-backed swap;
  - brightness, force-feedback and read-only power telemetry interfaces.

MicroPythonOS is not supported on this target.

FAT32 data storage and card hot-swap remain unqualified. Internal phone storage
writes, audio, modem, Wi-Fi and Linux reboot are not supported. Battery-only
power-off, physical-key wake, battery measurements and charging are unqualified.
Physical display, keypad and brightness effects remain unqualified on this
configuration. No physical vibration was observed under Linux without a battery.
Stock firmware can drive the motor, but the cause of the Linux limitation is
unknown; Linux vibration with a battery is unqualified.

TyrQuake game data can use RAM or ext4 microSD storage. Applications, files and
Bluetooth pairing records persist on the ext4 system root across cold boots.
Bluetooth requires firmware and configuration prepared from this exact phone;
see docs/features/BLUETOOTH.md for its shared use and limits.

Before ending a RAM session, stop applications using microSD, disable any
card-backed swap and unmount card filesystems as described in
docs/features/MICROSD.md. Then disconnect USB. If a battery is installed,
remove and reinsert it before booting the phone normally.
Before removing power from a microSD system root,
follow docs/guides/MICROSD_ROOT.md to establish that the card is safe to stop.
