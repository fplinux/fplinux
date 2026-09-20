FPLinux for INOI 244 Modern 4G

Read docs/guides/STANDALONE.md before connecting the phone. It contains the
host requirements, USB setup, checksum, loader-first and reconnect procedures.
This target's BootROM key is * (asterisk); hold it when the loader asks for the
powered-off phone.

Current target support:
  - local 240x320 terminal and physical keypad;
  - USB SSH/SFTP and host-keyboard forwarding;
  - microSD ext4 read/write, card-backed swap and a writable microSD system root;
  - RTC reading, setting, one-shot alarms and RTC-woken s2idle in both profiles,
    including mounted ext4 storage and card-backed swap;
  - Bluetooth pairing, bidirectional file transfer, PAN Internet and recovery
    after wake, with firmware prepared from this exact phone;
  - installable ARMADA and TyrQuake in both profiles;
  - installable MicroPythonOS launcher, navigation and keypad text input,
    with persistent state on the ext4 system root;
  - image rotation, JPEG codec/scaling and native presentation transfers.

The shared brightness, keypad-light, vibrator, telemetry and power-off
interfaces are enabled. Physical brightness, keypad light and native image
fidelity are unqualified. No physical vibration was observed under Linux
without a battery. Stock firmware can drive the motor, but the cause of the
Linux limitation is unknown; Linux vibration with a battery is unqualified.
Telemetry has been read with the battery absent; battery measurements, charging,
battery-only shutdown and physical-key wake remain unqualified.

Unmounted card hot-swap and FAT32 data storage are unqualified on this phone.
Internal phone storage writes, audio, modem, Wi-Fi and Linux reboot are not
supported.

The shared interfaces and storage safety rules are described in:
  - docs/features/AUXADC.md
  - docs/features/BATTERY_TELEMETRY.md
  - docs/features/CHARGER_STATUS.md
  - docs/features/DISPLAY_BACKLIGHT.md
  - docs/features/KEYPAD_BACKLIGHT.md
  - docs/features/MICROSD.md
  - docs/features/RTC.md
  - docs/features/POWER_OFF.md
  - docs/features/SOC_TEMPERATURE.md
  - docs/features/SUSPEND.md
  - docs/features/VIBRATION.md
  - docs/guides/MICROSD_ROOT.md

This phone uses backlight device inoi244-backlight, with configured levels
0 through 31 and default 31; power-supply devices sc2720-battery and
sc2720-charger; IIO name sc2720-auxadc; thermal-zone type ums9117-thm1;
and input name SC2720 vibrator. IIO, thermal and input-device
numbers are assigned at boot. These identifiers do not qualify the physical
effects or measurements noted above.

TyrQuake game data can use RAM or ext4 microSD storage. Applications, files,
MicroPythonOS state and Bluetooth pairing records persist on the ext4 system
root across cold boots. Optional MicroPythonOS FAT32 data-card storage is
unqualified.

Before ending a RAM-only session, stop applications using the card and follow
the safe-removal procedure in docs/features/MICROSD.md, including disabling
card-backed swap and unmounting card filesystems. Then disconnect USB. If a
battery is installed, remove and reinsert it before booting the phone normally.
Never use battery
removal to stop a writable microSD system root; follow the system-card safety
requirements in docs/guides/MICROSD_ROOT.md first.
