FPLinux for Nokia 3210 4G (TA-1618)

Read docs/guides/STANDALONE.md before connecting the phone. It contains the
host requirements, USB setup, checksum, loader-first and reconnect procedures.
This target's BootROM key is * (asterisk); hold it when the loader asks for the
powered-off phone.

An archive containing FPLINUX.img.xz boots with a persistent microSD root.
Prepare the system card first as described in docs/guides/MICROSD_ROOT.md.
An archive without that image uses the volatile RAM root. Both use the same
USB loader-first sequence and require a fresh USB load after power-off.

Current target support:
  - local 240x320 terminal with 11 LCD backlight levels;
  - physical keypad and keypad backlight;
  - bounded vibration through the Linux force-feedback interface;
  - USB SSH/SFTP and host-keyboard forwarding;
  - stereo S16_LE playback through the 3.5 mm headphones, with fitted gain
    levels and a flat VBC path;
  - microSD FAT32 read/write and unmounted hot-swap;
  - external charger connection status;
  - battery voltage, current and relative charge counter reporting with the
    documented accuracy limits;
  - optional per-command charge measurement through the bundled APK;
  - optional FPLinux: ARMADA display, keypad-light and vibration showcase;
  - hardware baseline JPEG decoding, fixed quality-85 JPEG encoding at
    1200x32, 320x240 and 640x480, and fixed half-size NV16 scaling;
  - native 240x320 NV16 or CPU-converted RGB565 image presentation;
  - calibrated SoC temperature reporting without a thermal-control policy;
  - raw auxiliary ADC readings without unit conversion;
  - real-time clock reading, setting and one-shot alarms;
  - s2idle in both profiles, including Bluetooth, mounted data cards and
    card-backed swap; wake with the red handset key or an RTC alarm;
  - battery-only power-off.

Headphone playback uses the `aplay` and `amixer` tools included in the image.
Upload a two-channel signed 16-bit little-endian WAV file and use the default
ALSA device:

  ./runner/run.py --reconnect --upload ./audio.wav /tmp/audio.wav
  ./runner/run.py --reconnect --exec "amixer -c 0 cset name='Headphone Playback Volume' 3,3"
  ./runner/run.py --reconnect --exec 'aplay /tmp/audio.wav'

The default device converts input to the 48 kHz hardware rate. The fitted gain
levels come from this phone, but its stock VBC EQ is not implemented; Nokia
output therefore does not have stock tonal parity. See
docs/features/HEADPHONE_AUDIO.md for direct-device limits and idle silence.
Phone microphone, FM radio and speaker audio are outside this feature.

Interfaces, limits and safety procedures are bundled at:
  - docs/features/AUXADC.md
  - docs/features/BATTERY_TELEMETRY.md
  - docs/features/CHARGER_STATUS.md
  - docs/features/DISPLAY_BACKLIGHT.md
  - docs/features/HEADPHONE_AUDIO.md
  - docs/features/KEYPAD_BACKLIGHT.md
  - docs/features/MICROSD.md
  - docs/features/POWER_OFF.md
  - docs/features/RTC.md
  - docs/features/SOC_TEMPERATURE.md
  - docs/features/SUSPEND.md
  - docs/features/VIBRATION.md

This phone uses backlight device ta1618-backlight, with levels 0 through 10
and default 10. Shared interface names are sc2720-battery, sc2720-charger,
sc2720-auxadc, ums9117-thermal, sc27xx:vibrator and UMS9117 Headphones. IIO,
thermal and input-device numbers are assigned at boot.

Install and run the optional showcase by following:
  - docs/apps/SHOWCASE.md

The included JPEG codec and scaler limits are described in:
  - docs/apps/JPEG.md
Native image presentation formats and completion boundaries are described in:
  - docs/apps/PRESENT.md

Internal phone storage writes, modem, Wi-Fi, battery level, battery temperature,
charge control and Linux reboot are not supported. The bundled
application guides use the shared storage interface documented in
docs/features/MICROSD.md; this phone supports FAT32 removable data storage.

Before ending a RAM-only session, follow the safe-removal procedure in
docs/features/MICROSD.md. With a microSD system root, stop applications and leave
the card installed; do not unmount the root or cut power. Then exit SSH,
disconnect USB, make sure charger power is absent and hold the red handset key
continuously for five seconds. Wait for orderly shutdown to complete. The
refusal and recovery behavior is documented in docs/features/POWER_OFF.md.
