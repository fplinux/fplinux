# INOI 240 Modern 4G

## Identity

| Field    | Value                                               |
| -------- | --------------------------------------------------- |
| Target   | `inoi-240-modern-4g`                                |
| Device   | INOI 240 Modern 4G                                  |
| Platform | [Unisoc UMS9117](../../platforms/ums9117/README.md) |
| Boot     | USB-loaded RAM bootstrap; initramfs or microSD root |

## Status

This exact phone runs the `128×160` console interface, the private USB session,
ARMADA, TyrQuake and the shared graphics applications. MicroPythonOS launches,
but its UI is not fully adapted to this phone's screen. microSD ext4 storage
and a writable microSD system root work. Bluetooth pairing, bidirectional
file transfer and PAN Internet work in both profiles. RTC alarms wake the
phone from s2idle, including with mounted ext4 storage and card-backed swap;
applications, Bluetooth and graphics work again after wake.

Physical display, key and brightness effects remain unqualified on this
configuration. The force-feedback interface responds, but no physical
vibration was observed under Linux without a battery. The motor works in
stock firmware; the cause of the Linux limitation is unknown, and Linux
vibration with a battery is unqualified. Telemetry read without a battery
does not qualify battery operation, measurement accuracy or charging.

Status terms and limits shared by every phone are defined in the
[target index](../README.md#status-and-common-limits).

## Features

| Feature                                                           | Hardware | FPLinux       | This phone                                                                                                                                    |
| ----------------------------------------------------------------- | -------- | ------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| RAM boot                                                          | N/A      | Supported     | —                                                                                                                                             |
| Persistent boot                                                   | N/A      | Not supported | A USB-loaded RAM bootstrap is required for every Linux boot.                                                                                  |
| [Local console](../../docs/features/LOCAL_CONSOLE.md)             | Present  | Partial       | `128×160`; physical display and keys are unqualified.                                                                                         |
| [LCD backlight](../../docs/features/DISPLAY_BACKLIGHT.md)         | Present  | Partial       | Brightness interface enabled; physical brightness levels are unqualified.                                                                     |
| [Keypad backlight](../../docs/features/KEYPAD_BACKLIGHT.md)       | Unknown  | Partial       | LED control is available; physical effect is unqualified.                                                                                     |
| [USB networking](../../docs/features/USB_NETWORKING.md)           | Present  | Supported     | —                                                                                                                                             |
| [SSH access](../../docs/features/SSH.md)                          | N/A      | Supported     | —                                                                                                                                             |
| [File transfer](../../docs/features/FILE_TRANSFER.md)             | N/A      | Supported     | RAM and writable mounted ext4 microSD storage are valid destinations.                                                                         |
| [Host keyboard bridge](../../docs/features/HOST_KEYBOARD.md)      | N/A      | Supported     | —                                                                                                                                             |
| [CPU clock reporting](../../docs/features/CPU_CLOCK.md)           | N/A      | Supported     | —                                                                                                                                             |
| [AP DMAengine](../../platforms/ums9117/README.md#memory-copy-dma) | Present  | Partial       | Memory copies and the fixed ROTA request; see platform limits.                                                                                |
| [Image rotation](../../docs/apps/ROTATE.md)                       | Present  | Supported     | RGB, grayscale and two-plane YUV through V4L2 mem2mem.                                                                                        |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)                 | Present  | Supported     | Baseline decode, fixed encode geometries and fixed NV16 scaler pairs.                                                                         |
| [Native image presentation](../../docs/apps/PRESENT.md)           | Present  | Partial       | Native `128×160` NV16 or RGB565; physical display is unqualified.                                                                             |
| USB host mode                                                     | Unknown  | Not supported | —                                                                                                                                             |
| [Removable storage](../../docs/features/MICROSD.md)               | Present  | Partial       | ext4 read/write and card-backed swap work in the RAM profile; FAT32 data storage and hot-swap are unqualified.                                |
| [Removable system root](../../docs/guides/MICROSD_ROOT.md)        | Present  | Supported     | FAT32 boot files plus writable ext4 root; applications, files and Bluetooth pairing records persist across cold boots.                        |
| Internal phone storage                                            | Present  | Not supported | Bluetooth preparation uses a separate read-only path; no writes.                                                                              |
| Audio                                                             | Present  | Not supported | —                                                                                                                                             |
| Modem and mobile service                                          | Present  | Not supported | —                                                                                                                                             |
| [Bluetooth](../../docs/features/BLUETOOTH.md)                     | Present  | Partial       | Pairing, bidirectional OPP, PAN Internet and recovery after RTC-woken s2idle work in both profiles.                                           |
| Wi-Fi                                                             | Unknown  | Not supported | —                                                                                                                                             |
| Camera                                                            | Unknown  | Not supported | —                                                                                                                                             |
| [Charger status](../../docs/features/CHARGER_STATUS.md)           | Present  | Partial       | External-input status is available; battery charging is unqualified.                                                                          |
| [Battery telemetry](../../docs/features/BATTERY_TELEMETRY.md)     | Present  | Partial       | Voltage, current and charge counter respond; battery measurements are unqualified.                                                            |
| [SoC temperature](../../docs/features/SOC_TEMPERATURE.md)         | Present  | Partial       | Temperature readings are available; physical accuracy is unqualified.                                                                         |
| [Auxiliary ADC](../../docs/features/AUXADC.md)                    | Present  | Partial       | Raw-channel interface enabled; physical inputs and accuracy are unqualified.                                                                  |
| [Real-time clock](../../docs/features/RTC.md)                     | Present  | Partial       | Read/set time, one-shot alarms and RTC wake work in both profiles.                                                                            |
| Other battery functions                                           | Unknown  | Not supported | No battery level, temperature or charge control is provided.                                                                                  |
| [Vibration](../../docs/features/VIBRATION.md)                     | Present  | Partial       | Force-feedback interface responds; no physical vibration under Linux without a battery. Battery-powered operation is unqualified.             |
| Indicator LEDs                                                    | Unknown  | Not supported | —                                                                                                                                             |
| [Power-off](../../docs/features/POWER_OFF.md)                     | N/A      | Partial       | Establish card safety before disconnecting USB power; battery-only power-off is unqualified.                                                  |
| [Suspend](../../docs/features/SUSPEND.md)                         | N/A      | Partial       | Repeated RTC-woken s2idle works in both profiles, including mounted ext4 data storage and card-backed swap; physical-key wake is unqualified. |
| Reboot                                                            | N/A      | Not supported | —                                                                                                                                             |

## Applications

| Application                                             | FPLinux   | This phone                                                                                                              |
| ------------------------------------------------------- | --------- | ----------------------------------------------------------------------------------------------------------------------- |
| [FPLinux: ARMADA](../../docs/apps/SHOWCASE.md)          | Supported | Runs in both profiles; physical display and light effects are unqualified, and there is no vibration without a battery. |
| [TyrQuake](../../docs/apps/TYRQUAKE.md)                 | Supported | Game data can use RAM or ext4 microSD storage, including the system root.                                               |
| [MicroPythonOS](../../docs/apps/MICROPYTHONOS.md)       | Partial   | Launches on this phone; the UI is not fully adapted to its 128×160 screen.                                              |
| [Image rotation](../../docs/apps/ROTATE.md)             | Supported | Explicit CPU/ROTA selection and framebuffer preview.                                                                    |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)       | Supported | Hardware decode, fixed encode and fixed half-scaling.                                                                   |
| [Native image presentation](../../docs/apps/PRESENT.md) | Supported | Direct NV16 or CPU-converted RGB565 presentation.                                                                       |

## Hardware interfaces

The `128×160` `NV3023` panel uses LCM/DBI with polled transfer completion.
The configured LCD brightness range is `0` through `31`, with `31` as the
default. Physical brightness levels and keypad-light effects remain unqualified.

Use these identifiers with the linked shared interfaces. Numeric IIO, thermal
and input-device indices are assigned at boot.

| Interface                                                     | Identifier on this phone                  |
| ------------------------------------------------------------- | ----------------------------------------- |
| [LCD backlight](../../docs/features/DISPLAY_BACKLIGHT.md)     | `/sys/class/backlight/inoi240-backlight`  |
| [Battery telemetry](../../docs/features/BATTERY_TELEMETRY.md) | `/sys/class/power_supply/inoi240-battery` |
| [Charger status](../../docs/features/CHARGER_STATUS.md)       | `/sys/class/power_supply/inoi240-charger` |
| [Auxiliary ADC](../../docs/features/AUXADC.md)                | IIO `name`: `inoi240-sc2720-auxadc`       |
| [SoC temperature](../../docs/features/SOC_TEMPERATURE.md)     | Thermal-zone `type`: `inoi240-soc`        |
| [Vibration](../../docs/features/VIBRATION.md)                 | Input name: `INOI 240 Modern 4G vibrator` |

## Bluetooth

Follow the shared [firmware preparation procedure](../../docs/guides/BUILDING.md#prepare-bluetooth-firmware)
using firmware and configuration from this exact phone. See the shared
[Bluetooth limits](../../docs/features/BLUETOOTH.md#limits-and-persistence)
for peer compatibility, persistence and unqualified features.

## Load into RAM

Follow [Loading from a source checkout](../../docs/guides/LOADING.md). When the loader
requests the phone, hold `*` and connect it powered off.

For a persistent system, follow the shared
[microSD system-root procedure](../../docs/guides/MICROSD_ROOT.md).

## End the RAM session

Stop applications using microSD and follow the shared
[safe-removal procedure](../../docs/features/MICROSD.md#safe-removal), including
disabling card-backed swap and unmounting card filesystems. Then disconnect
USB. If a battery is installed, remove and reinsert it before booting the
phone normally.

A writable microSD system root requires the shared
[USB-powered shutdown procedure](../../docs/guides/MICROSD_ROOT.md#shutdown-with-usb-power)
before removing power. Battery-only Linux power-off is unqualified; Linux
reboot is not supported.

## Release boundary

Feature support above does not qualify an executable payload. A locally
packaged candidate is not a release; see
[Release archives](../../docs/guides/RELEASES.md).
