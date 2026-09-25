# INOI 244 Modern 4G

## Identity

| Field    | Value                                               |
| -------- | --------------------------------------------------- |
| Target   | `inoi-244-modern-4g`                                |
| Device   | INOI 244 Modern 4G                                  |
| Platform | [Unisoc UMS9117](../../platforms/ums9117/README.md) |
| Boot     | USB-loaded RAM bootstrap; initramfs or microSD root |

## Status

This exact phone runs a local `240×320` console, its physical keypad, the
private USB session, headphone and speaker PCM playback, microphone capture, FM
radio, ARMADA, TyrQuake, MicroPythonOS
and the shared graphics applications. microSD ext4 storage and a writable
microSD system root work. Bluetooth pairing, bidirectional file transfer and
PAN Internet work in both profiles. RTC alarms wake the phone from s2idle,
including with mounted ext4 storage and card-backed swap; applications,
Bluetooth and graphics work again after wake.

Physical LCD brightness and keypad-light effects have not been tested. This
phone has no separate vibration motor; FPLinux vibrates it through the rear
speaker, as stock firmware does, and mutes headphone audio meanwhile.
Telemetry has been
read with the battery absent; that does not demonstrate battery measurements or
charging. Battery-only power-off and physical-key wake have not been tested.

Status terms and limits shared by every phone are defined in the
[target index](../README.md#status-and-common-limits).

## Features

| Feature                                                            | Hardware | FPLinux       | This phone                                                                                                                                         |
| ------------------------------------------------------------------ | -------- | ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| RAM boot                                                           | N/A      | Supported     | —                                                                                                                                                  |
| Persistent boot                                                    | N/A      | Not supported | A USB-loaded RAM bootstrap is required for every Linux boot.                                                                                       |
| [Local console](../../docs/features/LOCAL_CONSOLE.md)              | Present  | Supported     | `240×320`.                                                                                                                                         |
| [LCD backlight](../../docs/features/DISPLAY_BACKLIGHT.md)          | Present  | Partial       | Brightness interface enabled; visible brightness levels have not been tested.                                                                      |
| [Keypad backlight](../../docs/features/KEYPAD_BACKLIGHT.md)        | Unknown  | Partial       | Shared LED control enabled; its physical effect has not been tested.                                                                               |
| [USB networking](../../docs/features/USB_NETWORKING.md)            | Present  | Supported     | —                                                                                                                                                  |
| [SSH access](../../docs/features/SSH.md)                           | N/A      | Supported     | —                                                                                                                                                  |
| [File transfer](../../docs/features/FILE_TRANSFER.md)              | N/A      | Supported     | RAM and a writable mounted microSD are valid destinations.                                                                                         |
| [Host keyboard bridge](../../docs/features/HOST_KEYBOARD.md)       | N/A      | Supported     | —                                                                                                                                                  |
| [CPU clock reporting](../../docs/features/CPU_CLOCK.md)            | N/A      | Supported     | —                                                                                                                                                  |
| [Manual CPU frequency selection](../../docs/features/CPU_CLOCK.md) | N/A      | Partial       | The 768 MHz and 1 GHz settings are in the target build; switching on this phone has not been physically tested.                                    |
| [AP DMAengine](../../platforms/ums9117/README.md#memory-copy-dma)  | Present  | Partial       | Memory copies and the fixed ROTA request; see platform limits.                                                                                     |
| [Image rotation](../../docs/apps/ROTATE.md)                        | Present  | Supported     | RGB, grayscale and two-plane YUV through V4L2 mem2mem.                                                                                             |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)                  | Present  | Supported     | Baseline decode, fixed encode geometries and fixed NV16 scaler pairs.                                                                              |
| [Native image presentation](../../docs/apps/PRESENT.md)            | Present  | Partial       | Native `240×320` NV16 or RGB565 transfers; visible image fidelity has not been checked.                                                            |
| USB host mode                                                      | Unknown  | Not supported | —                                                                                                                                                  |
| [Removable storage](../../docs/features/MICROSD.md)                | Present  | Partial       | ext4 read/write and card-backed swap work in the RAM profile; FAT32 data storage and hot-swap have not been tested.                                |
| [Removable system root](../../docs/guides/MICROSD_ROOT.md)         | Present  | Supported     | FAT32 boot files plus writable ext4 root; applications, files and Bluetooth pairing records persist across cold boots.                             |
| Internal phone storage                                             | Present  | Not supported | Normal builds and runs do not read or expose NAND; fitted-data preparation is separate and read-only.                                              |
| [Headphone audio](../../docs/features/HEADPHONE_AUDIO.md)          | Present  | Supported     | Stereo S16_LE playback; the default device converts input to 48 kHz, and the fitted profile supplies stock-derived levels, equalizer and ALC.      |
| [Speaker audio](../../docs/features/SPEAKER_AUDIO.md)              | Present  | Supported     | Rear loudspeaker with fitted gains, stock equalizer and ALC.                                                                                       |
| [Phone microphone](../../docs/features/MICROPHONE_AUDIO.md)        | Present  | Partial       | The built-in microphone records mono 48-kHz PCM; the wired-headset microphone has not been tested on this phone.                                   |
| [FM radio](../../docs/features/FM_RADIO.md)                        | Present  | Supported     | Frequency scan with the 3.5 mm cable as antenna and playback through the enabled outputs; candidates need listening to confirm.                    |
| Modem and mobile service                                           | Present  | Not supported | —                                                                                                                                                  |
| [Bluetooth](../../docs/features/BLUETOOTH.md)                      | Present  | Partial       | Pairing, bidirectional OPP, PAN Internet and recovery after RTC-woken s2idle work in both profiles.                                                |
| Wi-Fi                                                              | Unknown  | Not supported | —                                                                                                                                                  |
| Camera                                                             | Unknown  | Not supported | —                                                                                                                                                  |
| [Charger status](../../docs/features/CHARGER_STATUS.md)            | Present  | Partial       | Read-only status is available; battery charging has not been tested.                                                                               |
| [Battery telemetry](../../docs/features/BATTERY_TELEMETRY.md)      | Present  | Partial       | Interface responds without a battery; battery voltage/current accuracy has not been checked.                                                       |
| [SoC temperature](../../docs/features/SOC_TEMPERATURE.md)          | Present  | Partial       | Shared temperature interface enabled; physical accuracy has not been checked.                                                                      |
| [Auxiliary ADC](../../docs/features/AUXADC.md)                     | Present  | Partial       | Shared raw-channel interface enabled; physical inputs and accuracy have not been checked.                                                          |
| [Real-time clock](../../docs/features/RTC.md)                      | Present  | Partial       | Read/set time, one-shot alarms and RTC wake work in both profiles.                                                                                 |
| Other battery functions                                            | Unknown  | Not supported | No battery level, temperature or charge control is provided.                                                                                       |
| [Vibration](../../docs/features/VIBRATION.md)                      | Present  | Partial       | Rear speaker vibration; headphone audio is muted meanwhile, and one continuous pulse lasts at most about three seconds.                            |
| Indicator LEDs                                                     | Unknown  | Not supported | —                                                                                                                                                  |
| [Power-off](../../docs/features/POWER_OFF.md)                      | N/A      | Partial       | Establish card safety before disconnecting USB power; battery-only power-off has not been tested.                                                  |
| [Suspend](../../docs/features/SUSPEND.md)                          | N/A      | Partial       | Repeated RTC-woken s2idle works in both profiles, including mounted ext4 data storage and card-backed swap; physical-key wake has not been tested. |
| Reboot                                                             | N/A      | Not supported | —                                                                                                                                                  |

## Applications

| Application                                             | FPLinux   | This phone                                                                                    |
| ------------------------------------------------------- | --------- | --------------------------------------------------------------------------------------------- |
| [FPLinux: ARMADA](../../docs/apps/SHOWCASE.md)          | Supported | Runs in both profiles; physical light effects have not been tested.                           |
| [TyrQuake](../../docs/apps/TYRQUAKE.md)                 | Supported | Game data can use RAM or ext4 microSD storage, including the system root.                     |
| [MicroPythonOS](../../docs/apps/MICROPYTHONOS.md)       | Supported | State persists on the ext4 system root; optional FAT32 data-card storage has not been tested. |
| [Image rotation](../../docs/apps/ROTATE.md)             | Supported | Explicit CPU/ROTA selection and framebuffer preview.                                          |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)       | Supported | Hardware decode, fixed encode and fixed half-scaling.                                         |
| [Native image presentation](../../docs/apps/PRESENT.md) | Supported | Direct NV16 or CPU-converted RGB565 transfers; visible image fidelity has not been checked.   |

## Hardware interfaces

The `240×320` `NV3030` panel uses LCM/DBI with polled transfer completion.
The configured LCD brightness range is `0` through `31`, with `31` as the
default. Physical brightness levels and keypad-light effects have not been tested.

The target-specific [LCD backlight](../../docs/features/DISPLAY_BACKLIGHT.md)
is `/sys/class/backlight/inoi244-backlight`. Shared feature pages document the
common power, sensor, audio and vibration identifiers.

## Fitted device data

Follow the shared
[device-data preparation procedure](../../docs/guides/BUILDING.md#prepare-device-data)
using a NAND backup from this exact phone. It prepares Bluetooth firmware, FM
settings and the fitted audio profile.

## Load into RAM

Follow [Loading from a source checkout](../../docs/guides/LOADING.md). When the loader
requests the phone, hold `*` and connect it powered off.

For a persistent system, follow the shared
[microSD system-root procedure](../../docs/guides/MICROSD_ROOT.md).

## End the RAM session

In the default RAM profile, stop applications using microSD and follow the
shared [safe-removal procedure](../../docs/features/MICROSD.md#safe-removal),
including disabling card-backed swap and unmounting card filesystems. Then
disconnect USB. If a battery is installed, remove and reinsert it before
booting the phone normally.

Battery-only Linux power-off has not been tested. Do not use this battery-removal
sequence for a writable microSD system root; follow the shared
[USB-powered shutdown procedure](../../docs/guides/MICROSD_ROOT.md#shutdown-with-usb-power).
Linux reboot is not supported.

## Release boundary

Feature support above does not make an executable payload release-ready. A locally
packaged candidate is not a release; see
[Release archives](../../docs/guides/RELEASES.md).
