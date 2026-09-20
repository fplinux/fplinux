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

The optional MicroPythonOS APK launches on this target. Support is partial:
its UI is not fully adapted to the 128x160 screen. See docs/apps/MICROPYTHONOS.md
for installation and use.

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

The shared hardware interfaces and their limits are described in:
  - docs/features/AUXADC.md
  - docs/features/BATTERY_TELEMETRY.md
  - docs/features/CHARGER_STATUS.md
  - docs/features/DISPLAY_BACKLIGHT.md
  - docs/features/KEYPAD_BACKLIGHT.md
  - docs/features/SOC_TEMPERATURE.md
  - docs/features/VIBRATION.md

This phone uses backlight device inoi240-backlight, with configured levels
0 through 31 and default 31; power-supply devices sc2720-battery and
sc2720-charger; IIO name sc2720-auxadc; thermal-zone type ums9117-thm1;
and input name SC2720 vibrator. IIO, thermal and input-device
numbers are assigned at boot. These identifiers do not qualify the physical
effects or measurements noted above.

Before ending a RAM session, stop applications using microSD, disable any
card-backed swap and unmount card filesystems as described in
docs/features/MICROSD.md. Then disconnect USB. If a battery is installed,
remove and reinsert it before booting the phone normally.
Before removing power from a microSD system root,
follow docs/guides/MICROSD_ROOT.md to establish that the card is safe to stop.
