FPLinux for Nokia 3210 4G (TA-1618)

Read docs/guides/STANDALONE.md before connecting the phone. It contains the
host requirements, USB setup, checksum, loader-first and reconnect procedures.
This target's BootROM key is * (asterisk); hold it when the loader asks for the
powered-off phone.

Current target support:
  - local 240x320 terminal, physical keypad and keypad backlight;
  - bounded vibration through the Linux force-feedback interface;
  - USB SSH/SFTP and host-keyboard forwarding;
  - microSD FAT32 read/write and unmounted hot-swap;
  - external charger connection status;
  - battery voltage, current and relative charge counter reporting with the
    documented accuracy limits;
  - optional per-command charge measurement through the bundled APK;
  - calibrated SoC temperature reporting without a thermal-control policy;
  - raw auxiliary ADC readings without unit conversion;
  - read-only real-time clock;
  - battery-only power-off.

The exact Nokia interfaces, limits and safety procedures are bundled at:
  - docs/target/AUXADC.md
  - docs/target/BATTERY_TELEMETRY.md
  - docs/target/CHARGER_STATUS.md
  - docs/target/KEYPAD_BACKLIGHT.md
  - docs/target/MICROSD.md
  - docs/target/POWER_OFF.md
  - docs/target/RTC.md
  - docs/target/SOC_TEMPERATURE.md
  - docs/target/SUSPEND.md
  - docs/target/VIBRATION.md

Internal phone storage, audio, modem, Bluetooth, Wi-Fi, battery level, battery
temperature and charge control, and Linux reboot are not supported. The bundled
application guides use the Nokia-specific FAT32 storage contract documented in
docs/target/MICROSD.md.

Before ending the RAM session, follow the safe-removal procedure in
docs/target/MICROSD.md. Then exit SSH, disconnect USB, make sure charger power
is absent and hold the red handset key continuously for five seconds. The
refusal and recovery behavior is documented in docs/target/POWER_OFF.md.
