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
  - stereo S16_LE playback through the 3.5 mm headphones.

The shared brightness, keypad-light, vibrator, telemetry and power-off
interfaces are enabled. Physical brightness, keypad light and native image
fidelity have not been tested. No physical vibration was observed under Linux
without a battery. Stock firmware can drive the motor, but the cause of the
Linux limitation is unknown; Linux vibration with a battery has not been tested.
Telemetry has been read with the battery absent; battery measurements, charging,
battery-only shutdown and physical-key wake have not been tested.

Unmounted card hot-swap and FAT32 data storage have not been tested on this phone.
Internal phone storage writes, modem, Wi-Fi and Linux reboot are not supported.

Headphone playback uses the `aplay` and `amixer` tools already included in this
image. From the extracted archive, upload a known two-channel, signed 16-bit
little-endian WAV file and use the default ALSA device:

  ./runner/run.py --reconnect --upload ./audio.wav /tmp/audio.wav
  ./runner/run.py --reconnect --exec "amixer -c 0 cset name='Headphone Playback Volume' 3,3"
  ./runner/run.py --reconnect --exec 'aplay /tmp/audio.wav'

The default device converts input to the 48 kHz hardware rate. See
docs/features/HEADPHONE_AUDIO.md for fitted volume levels, direct-device limits,
idle silence and its power trade-off. Phone microphone, FM radio and speaker
audio remain outside this feature's support boundary.

The shared interfaces and storage safety rules are described in:
  - docs/features/AUXADC.md
  - docs/features/BATTERY_TELEMETRY.md
  - docs/features/CHARGER_STATUS.md
  - docs/features/DISPLAY_BACKLIGHT.md
  - docs/features/KEYPAD_BACKLIGHT.md
  - docs/features/MICROSD.md
  - docs/features/HEADPHONE_AUDIO.md
  - docs/features/RTC.md
  - docs/features/POWER_OFF.md
  - docs/features/SOC_TEMPERATURE.md
  - docs/features/SUSPEND.md
  - docs/features/VIBRATION.md
  - docs/guides/MICROSD_ROOT.md

This phone uses backlight device inoi244-backlight, with configured levels
0 through 31 and default 31. Shared interface names are sc2720-battery,
sc2720-charger, sc2720-auxadc, ums9117-thermal, sc27xx:vibrator and UMS9117
Headphones. IIO, thermal and input-device numbers are assigned at boot. These
identifiers do not demonstrate the physical effects or measurements noted above.

TyrQuake game data can use RAM or ext4 microSD storage. Applications, files,
MicroPythonOS state and Bluetooth pairing records persist on the ext4 system
root across cold boots. Optional MicroPythonOS FAT32 data-card storage has not
been tested.

Before ending a RAM-only session, stop applications using the card and follow
the safe-removal procedure in docs/features/MICROSD.md, including disabling
card-backed swap and unmounting card filesystems. Then disconnect USB. If a
battery is installed, remove and reinsert it before booting the phone normally.
Never use battery
removal to stop a writable microSD system root; follow the system-card safety
requirements in docs/guides/MICROSD_ROOT.md first.
