# Maxvi K15n4G

## Identity

| Field    | Value                                               |
| -------- | --------------------------------------------------- |
| Target   | `maxvi-k15n-4g`                                     |
| Device   | Maxvi K15n4G                                        |
| Platform | [Unisoc UMS9117](../../platforms/ums9117/README.md) |
| Boot     | USB-loaded RAM bootstrap; initramfs root            |

## Status

This target boots Linux in RAM with USB networking, SSH access and read-only
physical NAND backups. Its `240×320` display provides a graphical terminal with
physical keypad input, menus, multi-tap typing and scrollback. RGB565 and native
NV16 image presentation, LCD brightness and automatic keypad lighting are
available. Headphone stereo and mute work with three-contact TRS headphones;
speaker playback and built-in microphone recording are available. Automatic
headphone/speaker switching is not supported. USB-powered s2idle can wake from
the RTC alarm or red handset key and restore the display and USB session.

Bluetooth pairing, OPP file transfer, PAN Internet access and SBC playback to a
Linux PC are available with the limits below.
Camera capture provides native `240×320` NV16 through V4L2, with sensor
orientation and adjustable brightness. Color accuracy is unqualified.
FAT32 and ext4 microSD storage support reads and writes in the RAM profile.
Insert the card before boot and leave it installed for the entire powered
session; hot-swap and a microSD system root are not supported.

Status terms and limits shared by every phone are defined in the
[target index](../README.md#status-and-common-limits).

## Features

| Feature                                                            | Hardware | FPLinux       | This phone                                                                                                                                                                 |
| ------------------------------------------------------------------ | -------- | ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| RAM boot                                                           | N/A      | Supported     | —                                                                                                                                                                          |
| Persistent boot                                                    | N/A      | Not supported | Load the RAM bootstrap over USB for every Linux boot.                                                                                                                      |
| [Local console](../../docs/features/LOCAL_CONSOLE.md)              | Present  | Supported     | Physical keypad, multi-tap, menus, Help, scrollback and one-shot modifier combinations.                                                                                    |
| [LCD backlight](../../docs/features/DISPLAY_BACKLIGHT.md)          | Present  | Supported     | System levels `0…10`; level `0` turns the light off.                                                                                                                       |
| [Keypad backlight](../../docs/features/KEYPAD_BACKLIGHT.md)        | Present  | Supported     | Lights on key presses and turns off after five seconds, including after wake.                                                                                              |
| [USB networking](../../docs/features/USB_NETWORKING.md)            | Present  | Supported     | —                                                                                                                                                                          |
| [SSH access](../../docs/features/SSH.md)                           | N/A      | Supported     | —                                                                                                                                                                          |
| [File transfer](../../docs/features/FILE_TRANSFER.md)              | N/A      | Supported     | —                                                                                                                                                                          |
| [Host keyboard bridge](../../docs/features/HOST_KEYBOARD.md)       | N/A      | Unknown       | —                                                                                                                                                                          |
| [CPU clock reporting](../../docs/features/CPU_CLOCK.md)            | N/A      | Supported     | —                                                                                                                                                                          |
| [Manual CPU frequency selection](../../docs/features/CPU_CLOCK.md) | N/A      | Supported     | 768 MHz and 1 GHz.                                                                                                                                                         |
| [AP DMAengine](../../platforms/ums9117/README.md#memory-copy-dma)  | Unknown  | Unknown       | —                                                                                                                                                                          |
| [Image rotation](../../docs/apps/ROTATE.md)                        | Unknown  | Partial       | CPU rotation is available; the ROTA device is disabled.                                                                                                                    |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)                  | Present  | Partial       | Native-size encoding, horizontal 4:2:2 half-resolution decoding and fixed half-scaling are available.                                                                      |
| [Native image presentation](../../docs/apps/PRESENT.md)            | Present  | Partial       | RGB565 and native NV16 presentation; color accuracy is unqualified.                                                                                                        |
| USB host mode                                                      | Unknown  | Not supported | —                                                                                                                                                                          |
| [Removable storage](../../docs/features/MICROSD.md)                | Present  | Partial       | SDHC FAT32 and ext4 read/write; install the card before boot and keep it installed throughout the powered session. No hot-swap.                                            |
| [Removable system root](../../docs/guides/MICROSD_ROOT.md)         | Present  | Not supported | Only the RAM root is available.                                                                                                                                            |
| Internal phone storage                                             | Present  | Partial       | Read-only raw [NAND backup](../../docs/guides/DEVICE_DATA.md#save-a-nand-backup); no filesystem, writes or restore.                                                        |
| [Headphone audio](../../docs/features/HEADPHONE_AUDIO.md)          | Present  | Partial       | Stereo PCM and per-channel mute with three-contact TRS headphones; jack detection works, but automatic output routing is not supported. TRRS compatibility is unqualified. |
| [Speaker audio](../../docs/features/SPEAKER_AUDIO.md)              | Present  | Supported     | PCM playback through the built-in speaker.                                                                                                                                 |
| [Phone microphone](../../docs/features/MICROPHONE_AUDIO.md)        | Present  | Partial       | Built-in microphone recording and replay, mono S16_LE at 48 kHz and duplex capture; acoustic response and headset input are unqualified.                                   |
| [FM radio](../../docs/features/FM_RADIO.md)                        | Present  | Partial       | Seeking, tuning and headphone reception, with or without the battery installed; speaker, stereo output and sensitivity are unqualified.                                    |
| Modem and mobile service                                           | Unknown  | Not supported | —                                                                                                                                                                          |
| [Bluetooth](../../docs/features/BLUETOOTH.md)                      | Present  | Partial       | Pairing, OPP, PAN and SBC A2DP source with a Linux PC; physical headsets and HID are unqualified.                                                                          |
| Wi-Fi                                                              | Unknown  | Not supported | —                                                                                                                                                                          |
| [Camera](../../docs/features/CAMERA.md)                            | Present  | Partial       | BF3A01; native `240×320` NV16, sensor rotation `270°` and adjustable brightness. Color accuracy is unqualified.                                                            |
| [Charger status](../../docs/features/CHARGER_STATUS.md)            | Present  | Partial       | USB input detection; battery charging is unqualified.                                                                                                                      |
| [Battery telemetry](../../docs/features/BATTERY_TELEMETRY.md)      | Unknown  | Not supported | The fuel gauge is disabled.                                                                                                                                                |
| [SoC temperature](../../docs/features/SOC_TEMPERATURE.md)          | Unknown  | Not supported | The thermal device is disabled.                                                                                                                                            |
| [Auxiliary ADC](../../docs/features/AUXADC.md)                     | Present  | Partial       | Raw readings are available; channel calibration is unqualified.                                                                                                            |
| [Real-time clock](../../docs/features/RTC.md)                      | Present  | Partial       | Read/write and alarm wake from s2idle; battery retention is unqualified.                                                                                                   |
| Other battery functions                                            | Unknown  | Unknown       | —                                                                                                                                                                          |
| [Vibration](../../docs/features/VIBRATION.md)                      | Unknown  | Not supported | No vibration device is enabled.                                                                                                                                            |
| Indicator LEDs                                                     | Unknown  | Not supported | —                                                                                                                                                                          |
| [Power-off](../../docs/features/POWER_OFF.md)                      | N/A      | Unknown       | —                                                                                                                                                                          |
| [Suspend](../../docs/features/SUSPEND.md)                          | N/A      | Partial       | USB-powered s2idle with RTC or red-handset wake restores the display and USB session; mounted ext4 storage remains usable after RTC wake.                                  |
| Reboot                                                             | N/A      | Not supported | —                                                                                                                                                                          |

## Applications

| Application                                             | FPLinux       | This phone                                                                            |
| ------------------------------------------------------- | ------------- | ------------------------------------------------------------------------------------- |
| [FPLinux: ARMADA](../../docs/apps/SHOWCASE.md)          | Not supported | No force-feedback device is enabled.                                                  |
| [TyrQuake](../../docs/apps/TYRQUAKE.md)                 | Partial       | Game data can use RAM or ext4 microSD storage; phone gameplay controls are supported. |
| [Image rotation](../../docs/apps/ROTATE.md)             | Partial       | CPU path; ROTA is disabled.                                                           |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)       | Partial       | Available within the qualified sizes and operations above.                            |
| [Native image presentation](../../docs/apps/PRESENT.md) | Partial       | RGB565 and native NV16 presentation; color accuracy is unqualified.                   |

## Load into RAM

Follow [Loading from a source checkout](../../docs/guides/LOADING.md). When the
loader requests the phone, hold the left soft key and connect it powered off.

## End the RAM session

Flush and unmount card filesystems as described in
[Removable microSD storage](../../docs/features/MICROSD.md#safe-removal), then
disconnect USB. If a battery is installed, remove and reinsert it before booting
the phone normally.

## Release boundary

Feature support above does not make an executable payload release-ready. A locally
packaged candidate is not a release; see
[Release archives](../../docs/guides/RELEASES.md).
