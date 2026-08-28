# Nokia 3210 4G (TA-1618)

## Identity

| Field         | Value                                               |
| ------------- | --------------------------------------------------- |
| Target        | `nokia-ta1618`                                      |
| Device        | Nokia 3210 4G (TA-1618)                             |
| Hardware code | `TA-1618`                                           |
| Platform      | [Unisoc UMS9117](../../platforms/ums9117/README.md) |
| Boot          | USB-loaded RAM bootstrap; initramfs or microSD root |

## Status

This exact phone runs a local `240×320` console, its physical keypad and
backlight, the private USB session, removable microSD storage, a USB-loaded
microSD system root, charger status, partial telemetry, a read-only RTC and
battery-only power-off.

Status terms and limits shared by every phone are defined in the
[target index](../README.md#status-and-common-limits).

## Features

| Feature                                                           | Hardware | FPLinux       | This phone                                                      |
| ----------------------------------------------------------------- | -------- | ------------- | --------------------------------------------------------------- |
| RAM boot                                                          | N/A      | Supported     | —                                                               |
| Persistent boot                                                   | N/A      | Not supported | The RAM bootstrap must be loaded over USB for every Linux boot. |
| [Local console](../../docs/features/LOCAL_CONSOLE.md)             | Present  | Supported     | `240×320`.                                                      |
| [Keypad backlight](features/KEYPAD_BACKLIGHT.md)                  | Present  | Supported     | Binary LED control plus a bounded key-press light.              |
| [USB networking](../../docs/features/USB_NETWORKING.md)           | Present  | Supported     | —                                                               |
| [SSH access](../../docs/features/SSH.md)                          | N/A      | Supported     | —                                                               |
| [File transfer](../../docs/features/FILE_TRANSFER.md)             | N/A      | Supported     | RAM and a writable mounted microSD are valid destinations.      |
| [Host keyboard](../../docs/features/HOST_KEYBOARD.md)             | N/A      | Supported     | —                                                               |
| [CPU clock reporting](../../docs/features/CPU_CLOCK.md)           | N/A      | Supported     | —                                                               |
| USB host mode                                                     | Unknown  | Not supported | —                                                               |
| [microSD](features/MICROSD.md)                                    | Present  | Supported     | FAT32 read/write and unmounted hot-swap are exercised.          |
| [microSD system root](profiles/microsd-uboot/features/MICROSD.md) | Present  | Supported     | FAT32 FIT plus writable ext4; the system card stays installed.  |
| Internal phone storage                                            | Present  | Not supported | —                                                               |
| Audio                                                             | Present  | Not supported | —                                                               |
| Modem and mobile service                                          | Present  | Not supported | —                                                               |
| Bluetooth                                                         | Unknown  | Not supported | —                                                               |
| Wi-Fi                                                             | Unknown  | Not supported | —                                                               |
| Camera                                                            | Unknown  | Not supported | Installed sensor is not identified.                             |
| [Charger status](features/CHARGER_STATUS.md)                      | Present  | Supported     | Read-only external-input status.                                |
| [Battery telemetry](features/BATTERY_TELEMETRY.md)                | Present  | Partial       | Voltage and signed current; absolute accuracy is unchecked.     |
| [SoC temperature](features/SOC_TEMPERATURE.md)                    | Present  | Partial       | Calibrated reading without external accuracy validation.        |
| [Auxiliary ADC](features/AUXADC.md)                               | Present  | Partial       | Five raw channels without physical-unit conversion.             |
| [Real-time clock](features/RTC.md)                                | Present  | Partial       | Read-only clock without system-time synchronization.            |
| Other battery functions                                           | Present  | Not supported | No level, battery temperature or charge control is provided.    |
| Indicator LEDs / vibration                                        | Unknown  | Not supported | —                                                               |
| [Power-off](features/POWER_OFF.md)                                | N/A      | Supported     | Works only while external charger power is absent.              |
| [Suspend](features/SUSPEND.md)                                    | N/A      | Supported     | RAM boot only; s2idle wakes from the red handset key.           |
| Reboot                                                            | N/A      | Not supported | —                                                               |

## Applications

| Application                                       | FPLinux   | This phone                                                     |
| ------------------------------------------------- | --------- | -------------------------------------------------------------- |
| [TyrQuake](../../docs/apps/TYRQUAKE.md)           | Supported | Game data can use the supported microSD path.                  |
| [MicroPythonOS](../../docs/apps/MICROPYTHONOS.md) | Supported | Requires the TA-1618 companion package; state can use microSD. |

## Load into RAM

Follow [Loading from a source checkout](../../docs/guides/LOADING.md). When the loader
requests the phone, hold `*` and connect it powered off.

The hardware-qualified microSD system mode uses a separate build context and
the public `microsd` boot selector:

```sh
./fplinux build nokia-ta1618 --profile microsd-uboot
./fplinux run nokia-ta1618 --boot microsd
```

This does not change the supported default RAM-only path or the stock boot
chain. Follow the candidate's bundled microSD and shutdown instructions; its
system card is not hot-swappable while Linux is running.

## Development diagnostics

The default build includes inactive kprobe and `irqsoff` diagnostics. Enabling
them can destabilize the kernel and consume RAM; follow
[Hardware debugging](../../docs/guides/DEBUGGING.md).

## End the RAM session

Flush and unmount microSD first. Disconnect USB, make sure charger power is
absent, then hold the red handset key continuously for five seconds. Releasing
it early cancels the request. If the phone remains powered after shutdown
starts, remove and reinsert the battery before booting normally. See
[Power-off](features/POWER_OFF.md) for the complete boundary.

## Release boundary

Feature support above does not qualify an executable payload. A locally
packaged candidate is not a release; see
[Release archives](../../docs/guides/RELEASES.md).
