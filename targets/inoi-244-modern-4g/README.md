# INOI 244 Modern 4G

## Identity

| Field    | Value                                               |
| -------- | --------------------------------------------------- |
| Target   | `inoi-244-modern-4g`                                |
| Device   | INOI 244 Modern 4G                                  |
| Platform | [Unisoc UMS9117](../../platforms/ums9117/README.md) |
| Boot     | Volatile RAM only                                   |

## Status

This exact phone runs a local `240×320` console, its physical keypad, the
private USB session and the shared applications. No supported microSD,
keypad-backlight, battery or power-off path is available.

Status terms and limits shared by every phone are defined in the
[target index](../README.md#status-and-common-limits).

## Features

| Feature                                                      | Hardware | FPLinux       | This phone                                                   |
| ------------------------------------------------------------ | -------- | ------------- | ------------------------------------------------------------ |
| RAM boot                                                     | N/A      | Supported     | —                                                            |
| Persistent boot                                              | N/A      | Not supported | —                                                            |
| [Local console](../../docs/features/LOCAL_CONSOLE.md)        | Present  | Supported     | `240×320`.                                                   |
| LCD backlight                                                | Present  | Not supported | No user brightness interface is provided.                    |
| Keypad backlight                                             | Unknown  | Not supported | No FPLinux control is provided.                              |
| [USB networking](../../docs/features/USB_NETWORKING.md)      | Present  | Supported     | —                                                            |
| [SSH access](../../docs/features/SSH.md)                     | N/A      | Supported     | —                                                            |
| [File transfer](../../docs/features/FILE_TRANSFER.md)        | N/A      | Supported     | Destinations are RAM-backed on this target.                  |
| [Host keyboard bridge](../../docs/features/HOST_KEYBOARD.md) | N/A      | Supported     | —                                                            |
| [CPU clock reporting](../../docs/features/CPU_CLOCK.md)      | N/A      | Supported     | —                                                            |
| USB host mode                                                | Unknown  | Not supported | —                                                            |
| Removable storage                                            | Unknown  | Not supported | No supported microSD path is provided.                       |
| Removable system root                                        | Unknown  | Not supported | —                                                            |
| Internal phone storage                                       | Present  | Not supported | —                                                            |
| Audio                                                        | Present  | Not supported | —                                                            |
| Modem and mobile service                                     | Present  | Not supported | —                                                            |
| Bluetooth                                                    | Unknown  | Not supported | —                                                            |
| Wi-Fi                                                        | Unknown  | Not supported | —                                                            |
| Camera                                                       | Unknown  | Not supported | —                                                            |
| Charger status                                               | Unknown  | Not supported | No external-input status is exposed.                         |
| Battery telemetry                                            | Unknown  | Not supported | No voltage or current reporting is provided.                 |
| SoC temperature                                              | Unknown  | Not supported | —                                                            |
| Auxiliary ADC                                                | Unknown  | Not supported | —                                                            |
| Real-time clock                                              | Unknown  | Not supported | —                                                            |
| Other battery functions                                      | Unknown  | Not supported | No battery level, temperature or charge control is provided. |
| Vibration                                                    | Unknown  | Not supported | —                                                            |
| Indicator LEDs                                               | Unknown  | Not supported | —                                                            |
| Power-off                                                    | N/A      | Not supported | End the session by reseating the battery as described below. |
| Suspend                                                      | N/A      | Not supported | —                                                            |
| Reboot                                                       | N/A      | Not supported | —                                                            |

## Applications

| Application                                       | FPLinux   | This phone                                                 |
| ------------------------------------------------- | --------- | ---------------------------------------------------------- |
| [TyrQuake](../../docs/apps/TYRQUAKE.md)           | Supported | Game data uses tmpfs and consumes the phone's limited RAM. |
| [MicroPythonOS](../../docs/apps/MICROPYTHONOS.md) | Supported | Application state remains in RAM.                          |

## Load into RAM

Follow [Loading from a source checkout](../../docs/guides/LOADING.md). When the loader
requests the phone, hold `*` and connect it powered off.

## End the RAM session

Disconnect USB, remove and reinsert the battery, then boot the phone normally.
This target does not provide Linux power-off or reboot.

## Release boundary

Feature support above does not qualify an executable payload. A locally
packaged candidate is not a release; see
[Release archives](../../docs/guides/RELEASES.md).
