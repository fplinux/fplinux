FPLinux for INOI 240 Modern 4G

Read docs/guides/STANDALONE.md before connecting the phone. It contains the
host requirements, USB setup, checksum, loader-first and reconnect procedures.
This target's BootROM key is * (asterisk); hold it when the loader asks for the
powered-off phone.

Current target support:
  - 128x160 terminal interface;
  - USB SSH/SFTP and host-keyboard forwarding;
  - ARMADA, TyrQuake, image rotation, JPEG codec/scaling and presentation;
  - microSD FAT32 and ext4 read/write, card-backed swap and a writable microSD
    system root;
  - Bluetooth pairing, bidirectional file transfer and PAN Internet in both
    the default RAM and microsd-uboot profiles;
  - RTC time and alarms, repeated RTC-woken s2idle and peripheral recovery in
    both profiles, including mounted ext4 storage and card-backed swap;
  - stereo S16_LE WAV playback through the 3.5 mm headphones;
  - PCM playback through the built-in speaker, alone or with the headphones;
  - mono 48 kHz recording from the built-in or wired-headset microphone;
  - FM scan through the 3.5 mm jack and playback through the enabled audio
    outputs;
  - keypad backlight on and off;
  - manual CPU frequency selection between 768 MHz and 1 GHz;
  - brightness and read-only power telemetry interfaces.

The optional MicroPythonOS APK launches on this target. Support is partial:
its UI is not fully adapted to the 128x160 screen. See docs/apps/MICROPYTHONOS.md
for installation and use.

The microSD slot is under the battery, so card hot-swap does not apply.
Internal phone storage writes, modem, Wi-Fi and Linux reboot are not supported.
Battery-only power-off, physical-key wake, battery measurements and charging
have not been tested. Physical display, key and LCD brightness effects have not
been tested on this configuration. This phone has no separate vibration motor;
FPLinux vibrates it through the speaker and mutes headphone audio
meanwhile, as described in docs/features/VIBRATION.md.

Headphone playback uses the `aplay` and `amixer` tools already included in this
image. From the extracted archive, upload a known two-channel, signed 16-bit
little-endian WAV file and use the default ALSA device:

  ./runner/run.py --reconnect --upload ./audio.wav /tmp/audio.wav
  ./runner/run.py --reconnect --exec "amixer -c 0 cset name='Headphone Playback Volume' 3,3"
  ./runner/run.py --reconnect --exec 'aplay /tmp/audio.wav'

The default device converts input to the 48 kHz hardware rate. Use
`amixer -c 0 cget name='Headphone Playback Volume'` to inspect the active volume
control. See docs/features/HEADPHONE_AUDIO.md for fitted volume levels,
direct-device limits, EQ bypass, on-device calibration, idle silence and its
power trade-off. Enable the built-in speaker with `Speaker Playback Switch`;
see docs/features/SPEAKER_AUDIO.md.
Select `Capture Source` before microphone recording; see
docs/features/MICROPHONE_AUDIO.md. FM needs a wired 3.5 mm cable as its antenna;
see docs/features/FM_RADIO.md for scan and playback commands.

TyrQuake game data can use RAM or ext4 microSD storage. Applications, files and
Bluetooth pairing records persist on the ext4 system root across cold boots.
Bluetooth requires firmware and configuration prepared from this exact phone;
see docs/features/BLUETOOTH.md for its shared use and limits.

The shared interfaces, limits and safety procedures are described in:
  - docs/features/AUXADC.md
  - docs/features/BATTERY_TELEMETRY.md
  - docs/features/BLUETOOTH.md
  - docs/features/CHARGER_STATUS.md
  - docs/features/CPU_CLOCK.md
  - docs/features/DISPLAY_BACKLIGHT.md
  - docs/features/FM_RADIO.md
  - docs/features/HEADPHONE_AUDIO.md
  - docs/features/KEYPAD_BACKLIGHT.md
  - docs/features/MICROPHONE_AUDIO.md
  - docs/features/MICROSD.md
  - docs/features/POWER_OFF.md
  - docs/features/RTC.md
  - docs/features/SOC_TEMPERATURE.md
  - docs/features/SPEAKER_AUDIO.md
  - docs/features/SUSPEND.md
  - docs/features/VIBRATION.md
  - docs/guides/MICROSD_ROOT.md

This phone uses backlight device inoi240-backlight, with configured levels
0 through 31 and default 31. Shared interface names are sc2720-battery,
sc2720-charger, sc2720-auxadc, ums9117-thermal, UMS9117 speaker vibrator and
UMS9117 Headphones. IIO, thermal and input-device numbers are assigned at boot. These
identifiers do not demonstrate the physical effects or measurements noted above.

Before ending a RAM session, stop applications using microSD, disable any
card-backed swap and unmount card filesystems as described in
docs/features/MICROSD.md. Then disconnect USB. If a battery is installed,
remove and reinsert it before booting the phone normally.
Before removing power from a microSD system root,
follow docs/guides/MICROSD_ROOT.md to establish that the card is safe to stop.
