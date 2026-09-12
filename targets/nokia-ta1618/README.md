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

This exact phone runs a local `240×320` console with adjustable LCD backlight,
its physical keypad and backlight, the private USB session, removable microSD
storage, a USB-loaded microSD system root, charger status, partial telemetry,
RTC time reading and setting, one-shot alarms and battery-only power-off.
Its vibrator is available through the standard Linux force-feedback interface.

Status terms and limits shared by every phone are defined in the
[target index](../README.md#status-and-common-limits).

## Features

| Feature                                                           | Hardware | FPLinux       | This phone                                                                                                                                  |
| ----------------------------------------------------------------- | -------- | ------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| RAM boot                                                          | N/A      | Supported     | —                                                                                                                                           |
| Persistent boot                                                   | N/A      | Not supported | The RAM bootstrap must be loaded over USB for every Linux boot.                                                                             |
| [Local console](../../docs/features/LOCAL_CONSOLE.md)             | Present  | Supported     | `240×320`; AP DMA framebuffer copies.                                                                                                       |
| [LCD backlight](../../docs/features/DISPLAY_BACKLIGHT.md)         | Present  | Supported     | Eleven levels from off through the qualified maximum.                                                                                       |
| [Keypad backlight](../../docs/features/KEYPAD_BACKLIGHT.md)       | Present  | Supported     | Binary LED control plus a bounded key-press light.                                                                                          |
| [USB networking](../../docs/features/USB_NETWORKING.md)           | Present  | Supported     | —                                                                                                                                           |
| [SSH access](../../docs/features/SSH.md)                          | N/A      | Supported     | —                                                                                                                                           |
| [File transfer](../../docs/features/FILE_TRANSFER.md)             | N/A      | Supported     | RAM and a writable mounted microSD are valid destinations.                                                                                  |
| [Host keyboard bridge](../../docs/features/HOST_KEYBOARD.md)      | N/A      | Supported     | —                                                                                                                                           |
| [CPU clock reporting](../../docs/features/CPU_CLOCK.md)           | N/A      | Supported     | —                                                                                                                                           |
| [AP DMAengine](../../platforms/ums9117/README.md#memory-copy-dma) | Present  | Partial       | Framebuffer copies and the fixed ROTA request share one controller; see platform limits.                                                    |
| [Image rotation](../../docs/apps/ROTATE.md)                       | Present  | Supported     | Hardware rotation through V4L2 mem2mem, including RGB, grayscale and two-plane YUV.                                                         |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)                 | Present  | Supported     | Baseline JPEG decode, three fixed JPEG encode geometries, and two fixed half-size NV16 scaler pairs.                                        |
| [Native image presentation](../../docs/apps/PRESENT.md)           | Present  | Supported     | Native `240×320` NV16 or CPU-converted RGB565 presentation through LCDC.                                                                    |
| USB host mode                                                     | Unknown  | Not supported | —                                                                                                                                           |
| [Removable storage](../../docs/features/MICROSD.md)               | Present  | Supported     | microSD FAT32 read/write and unmounted hot-swap are exercised.                                                                              |
| [Removable system root](../../docs/guides/MICROSD_ROOT.md)        | Present  | Supported     | microSD FAT32 FIT plus writable ext4; the system card stays installed.                                                                      |
| Internal phone storage                                            | Present  | Not supported | Default and release workflows expose none. The Bluetooth preparation path is a separate read-only diagnostic.                               |
| Audio                                                             | Present  | Not supported | —                                                                                                                                           |
| Modem and mobile service                                          | Present  | Not supported | —                                                                                                                                           |
| [Bluetooth](../../docs/features/BLUETOOTH.md)                     | Present  | Partial       | BR/EDR pairing, bidirectional OPP and PAN Internet in both profiles; requires firmware from this phone.                                     |
| Wi-Fi                                                             | Unknown  | Not supported | —                                                                                                                                           |
| Camera                                                            | Unknown  | Not supported | Installed sensor is not identified.                                                                                                         |
| [Charger status](../../docs/features/CHARGER_STATUS.md)           | Present  | Supported     | Read-only external-input status.                                                                                                            |
| [Battery telemetry](../../docs/features/BATTERY_TELEMETRY.md)     | Present  | Partial       | Voltage, signed current and a relative charge counter; absolute accuracy is unchecked.                                                      |
| [SoC temperature](../../docs/features/SOC_TEMPERATURE.md)         | Present  | Partial       | Calibrated reading without external accuracy validation.                                                                                    |
| [Auxiliary ADC](../../docs/features/AUXADC.md)                    | Present  | Partial       | Five raw channels without physical-unit conversion.                                                                                         |
| [Real-time clock](../../docs/features/RTC.md)                     | Present  | Partial       | Read/set time and one-shot alarms; RTC wake works in both profiles, including after time correction.                                        |
| Other battery functions                                           | Present  | Not supported | No level, battery temperature or charge control is provided.                                                                                |
| [Vibration](../../docs/features/VIBRATION.md)                     | Present  | Supported     | Binary `FF_RUMBLE` effects with a five-second automatic cutoff.                                                                             |
| Indicator LEDs                                                    | Unknown  | Not supported | —                                                                                                                                           |
| [Power-off](../../docs/features/POWER_OFF.md)                     | N/A      | Supported     | Hold the red handset key for five seconds; external charger power must be absent.                                                           |
| [Suspend](../../docs/features/SUSPEND.md)                         | N/A      | Supported     | s2idle in both profiles, including Bluetooth, mounted data cards and card-backed swap; the red handset key and RTC alarms are wake sources. |
| Reboot                                                            | N/A      | Not supported | —                                                                                                                                           |

## Applications

| Application                                             | FPLinux   | This phone                                                                                     |
| ------------------------------------------------------- | --------- | ---------------------------------------------------------------------------------------------- |
| [FPLinux: ARMADA](../../docs/apps/SHOWCASE.md)          | Supported | Synchronizes the display, keypad light and vibrator.                                           |
| [TyrQuake](../../docs/apps/TYRQUAKE.md)                 | Supported | Game data can use the supported microSD path.                                                  |
| [MicroPythonOS](../../docs/apps/MICROPYTHONOS.md)       | Supported | State can use FAT32 microSD with the shared optional storage package.                          |
| [Image rotation](../../docs/apps/ROTATE.md)             | Supported | Included in the normal root filesystem; explicit CPU/ROTA selection and framebuffer preview.   |
| [JPEG codec and scaling](../../docs/apps/JPEG.md)       | Supported | Included in the normal root filesystem; hardware decode, fixed encode, and fixed half-scaling. |
| [Native image presentation](../../docs/apps/PRESENT.md) | Supported | Included in the normal root filesystem; direct NV16 or CPU-converted RGB565 presentation.      |

## Hardware interfaces

The `240×320` `ST7789P3` panel uses SPI with interrupt-driven transfer completion.
LCD levels `1` through `10` increase monotonically up to the qualified maximum;
`10` is the default. They are raw current steps rather than percentages or
calibrated optical units.

Use these identifiers with the linked shared interfaces. Numeric IIO, thermal
and input-device indices are assigned at boot.

| Interface                                                     | Identifier on this phone                 |
| ------------------------------------------------------------- | ---------------------------------------- |
| [LCD backlight](../../docs/features/DISPLAY_BACKLIGHT.md)     | `/sys/class/backlight/ta1618-backlight`  |
| [Battery telemetry](../../docs/features/BATTERY_TELEMETRY.md) | `/sys/class/power_supply/ta1618-battery` |
| [Charger status](../../docs/features/CHARGER_STATUS.md)       | `/sys/class/power_supply/ta1618-charger` |
| [Auxiliary ADC](../../docs/features/AUXADC.md)                | IIO `name`: `ta1618-sc2720-auxadc`       |
| [SoC temperature](../../docs/features/SOC_TEMPERATURE.md)     | Thermal-zone `type`: `ta1618-soc`        |
| [Vibration](../../docs/features/VIBRATION.md)                 | Input name: `TA-1618 vibrator`           |

## Bluetooth

Both profiles use the shared
[Bluetooth preparation procedure](../../docs/guides/BUILDING.md#prepare-bluetooth-firmware)
with firmware from this exact phone. See the shared
[Bluetooth limits](../../docs/features/BLUETOOTH.md#limits-and-persistence)
for peer compatibility, persistence and unqualified features.

## Load into RAM

Follow [Loading from a source checkout](../../docs/guides/LOADING.md). When the loader
requests the phone, hold `*` and connect it powered off.

For a persistent system, follow the shared
[microSD system-root procedure](../../docs/guides/MICROSD_ROOT.md).

## Development diagnostics

The default build includes inactive kprobe and `irqsoff` diagnostics. Enabling
them can destabilize the kernel and consume RAM; follow
[Hardware debugging](../../docs/guides/DEBUGGING.md).

## End the RAM session

Follow the shared [power-off procedure](../../docs/features/POWER_OFF.md).
The physical power key is the red handset key. A short press of this key also
wakes the phone from s2idle; `8` and the matrix keypad do not wake it.

## Release boundary

Feature support above does not qualify an executable payload. A locally
packaged candidate is not a release; see
[Release archives](../../docs/guides/RELEASES.md).
