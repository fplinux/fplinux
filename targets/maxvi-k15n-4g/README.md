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
physical NAND backups. Its `240×320` display provides a graphical terminal and
RGB565 image presentation. LCD brightness, manual keypad lighting, headphone
playback, built-in microphone capture and RTC-woken s2idle are available with
the limits below.

Physical keypad input and red-handset wake remain unqualified; use the USB
session for control. Bluetooth pairing, OPP file transfer, PAN Internet access
and SBC playback to a Linux PC are available with the limits below.
Speaker output is unqualified.
Camera capture provides native `240×320` NV16 through V4L2, with sensor
orientation and adjustable brightness. Color accuracy is unqualified.
MicroSD access is not supported.

Status terms and limits shared by every phone are defined in the
[target index](../README.md#status-and-common-limits).

## Features

| Feature                                                            | Hardware | FPLinux       | This phone                                                                                                          |
| ------------------------------------------------------------------ | -------- | ------------- | ------------------------------------------------------------------------------------------------------------------- |
| RAM boot                                                           | N/A      | Supported     | —                                                                                                                   |
| Persistent boot                                                    | N/A      | Not supported | Load the RAM bootstrap over USB for every Linux boot.                                                               |
| [Local console](../../docs/features/LOCAL_CONSOLE.md)              | Present  | Partial       | `240×320` graphical terminal; physical keypad input is unqualified.                                                 |
| [LCD backlight](../../docs/features/DISPLAY_BACKLIGHT.md)          | Present  | Partial       | System levels `0…10`; level `0` turns the light off. Visible step spacing is unqualified.                           |
| [Keypad backlight](../../docs/features/KEYPAD_BACKLIGHT.md)        | Present  | Partial       | Manual binary control; automatic key-press lighting is unqualified.                                                 |
| [USB networking](../../docs/features/USB_NETWORKING.md)            | Present  | Supported     | —                                                                                                                   |
| [SSH access](../../docs/features/SSH.md)                           | N/A      | Supported     | —                                                                                                                   |
| [File transfer](../../docs/features/FILE_TRANSFER.md)              | N/A      | Supported     | —                                                                                                                   |
| [Host keyboard bridge](../../docs/features/HOST_KEYBOARD.md)       | N/A      | Unknown       | —                                                                                                                   |
| [CPU clock reporting](../../docs/features/CPU_CLOCK.md)            | N/A      | Supported     | —                                                                                                                   |
| [Manual CPU frequency selection](../../docs/features/CPU_CLOCK.md) | N/A      | Supported     | 768 MHz and 1 GHz.                                                                                                  |
| [AP DMAengine](../../platforms/ums9117/README.md#memory-copy-dma)  | Unknown  | Unknown       | —                                                                                                                   |
| [Image rotation](../../docs/apps/ROTATE.md)                        | Unknown  | Partial       | CPU rotation is available; the ROTA device is disabled.                                                             |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)                  | Present  | Partial       | Native-size encoding, horizontal 4:2:2 half-resolution decoding and fixed half-scaling are available.               |
| [Native image presentation](../../docs/apps/PRESENT.md)            | Present  | Partial       | RGB565 presentation; native NV16 colors remain unqualified.                                                         |
| USB host mode                                                      | Unknown  | Not supported | —                                                                                                                   |
| [Removable storage](../../docs/features/MICROSD.md)                | Present  | Not supported | No supported microSD access.                                                                                        |
| [Removable system root](../../docs/guides/MICROSD_ROOT.md)         | Present  | Not supported | Only the RAM root is available.                                                                                     |
| Internal phone storage                                             | Present  | Partial       | Read-only raw [NAND backup](../../docs/guides/DEVICE_DATA.md#save-a-nand-backup); no filesystem, writes or restore. |
| [Headphone audio](../../docs/features/HEADPHONE_AUDIO.md)          | Present  | Partial       | PCM playback; stereo separation and automatic jack routing are unqualified.                                         |
| [Speaker audio](../../docs/features/SPEAKER_AUDIO.md)              | Present  | Unknown       | Physical output is unqualified.                                                                                     |
| [Phone microphone](../../docs/features/MICROPHONE_AUDIO.md)        | Present  | Partial       | Mono S16_LE at 48 kHz and duplex capture are available; acoustic response and headset input are unqualified.        |
| [FM radio](../../docs/features/FM_RADIO.md)                        | Present  | Partial       | Seeking, tuning and headphone audio are available; speaker and stereo output are unqualified.                       |
| Modem and mobile service                                           | Unknown  | Not supported | —                                                                                                                   |
| [Bluetooth](../../docs/features/BLUETOOTH.md)                      | Present  | Partial       | Pairing, OPP, PAN and SBC A2DP source with a Linux PC; physical headsets and HID are unqualified.                   |
| Wi-Fi                                                              | Unknown  | Not supported | —                                                                                                                   |
| [Camera](../../docs/features/CAMERA.md)                            | Present  | Partial       | BF3A01; native `240×320` NV16, sensor rotation `270°` and adjustable brightness. Color accuracy is unqualified.     |
| [Charger status](../../docs/features/CHARGER_STATUS.md)            | Present  | Partial       | USB input detection; battery charging is unqualified.                                                               |
| [Battery telemetry](../../docs/features/BATTERY_TELEMETRY.md)      | Unknown  | Not supported | The fuel gauge is disabled.                                                                                         |
| [SoC temperature](../../docs/features/SOC_TEMPERATURE.md)          | Unknown  | Not supported | The thermal device is disabled.                                                                                     |
| [Auxiliary ADC](../../docs/features/AUXADC.md)                     | Present  | Partial       | Raw readings are available; channel calibration is unqualified.                                                     |
| [Real-time clock](../../docs/features/RTC.md)                      | Present  | Partial       | Read/write and alarm wake from s2idle; battery retention is unqualified.                                            |
| Other battery functions                                            | Unknown  | Unknown       | —                                                                                                                   |
| [Vibration](../../docs/features/VIBRATION.md)                      | Unknown  | Not supported | No vibration device is enabled.                                                                                     |
| Indicator LEDs                                                     | Unknown  | Not supported | —                                                                                                                   |
| [Power-off](../../docs/features/POWER_OFF.md)                      | N/A      | Unknown       | —                                                                                                                   |
| [Suspend](../../docs/features/SUSPEND.md)                          | N/A      | Partial       | USB-powered s2idle with RTC wake restores the display and USB session; red-handset wake is unqualified.             |
| Reboot                                                             | N/A      | Not supported | —                                                                                                                   |

## Applications

| Application                                             | FPLinux       | This phone                                                      |
| ------------------------------------------------------- | ------------- | --------------------------------------------------------------- |
| [FPLinux: ARMADA](../../docs/apps/SHOWCASE.md)          | Not supported | No force-feedback device is enabled.                            |
| [TyrQuake](../../docs/apps/TYRQUAKE.md)                 | Partial       | RAM-backed game data; physical controls are unqualified.        |
| [Image rotation](../../docs/apps/ROTATE.md)             | Partial       | CPU path; ROTA is disabled.                                     |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)       | Partial       | Available within the qualified sizes and operations above.      |
| [Native image presentation](../../docs/apps/PRESENT.md) | Partial       | RGB565 presentation; native NV16 color fidelity is unqualified. |

## Load into RAM

Follow [Loading from a source checkout](../../docs/guides/LOADING.md). When the
loader requests the phone, hold the left soft key and connect it powered off.

## End the RAM session

Disconnect USB. If a battery is installed, remove and reinsert it before
booting the phone normally.

## Release boundary

Feature support above does not make an executable payload release-ready. A locally
packaged candidate is not a release; see
[Release archives](../../docs/guides/RELEASES.md).
