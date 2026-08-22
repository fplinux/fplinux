# Nokia 3210 4G (TA-1618)

## Device

| Field    | Value                                                      |
| -------- | ---------------------------------------------------------- |
| Device   | Nokia 3210 4G (TA-1618)                                    |
| Platform | [Unisoc UMS9117 / T117](../../platforms/ums9117/README.md) |
| Profile  | `console`                                                  |
| Boot     | Volatile RAM only                                          |

## Status

This target provides a local `240×320` terminal, physical keypad and keypad
backlight, microSD access, and a cold-owned USB peripheral profile on a
TA-1618. That profile has booted without loader-inherited USB state, enumerated
at High-Speed, exposed both `g_serial` ports, completed a host shell/data pull,
and survived physical USB disconnect/reconnect. The Linux session is volatile
and internal phone storage stays inaccessible.

## Evidence basis

Supported entries record physical development observations on this exact phone:
boot, kernel, screen, keypad, backlight, microSD, battery-only power-off and
the USB observations in the status above have been exercised. MicroPythonOS
launcher navigation, keypad text input, File Manager, card-backed state, synced
write, read after remount and clean unmount have also been exercised. USB upload,
host-keyboard forwarding and application install/use have not been exercised on
the current cold-owned profile. These observations are not a release
qualification record and do not identify a qualified candidate or executable
payload.

## Hardware support

**Hardware** records only what is established for this phone; **Unknown** does
not mean absent. **FPLinux** is **Supported** only after exercise on this exact
variant. **Not supported** describes the current target, not the hardware.

| Area                       | Hardware | FPLinux       | What a user can rely on / limit                                                       |
| -------------------------- | -------- | ------------- | ------------------------------------------------------------------------------------- |
| RAM boot                   | N/A      | Supported     | Linux runs only in RAM and does not write phone storage.                              |
| Persistent boot            | N/A      | Not supported | No autonomous Linux boot path is provided.                                            |
| Local screen               | Present  | Supported     | `240×320` framebuffer terminal.                                                       |
| Physical keypad            | Present  | Supported     | Local terminal, TyrQuake and MicroPythonOS input.                                     |
| Keypad backlight           | Present  | Supported     | A key press lights it for about five seconds; manual control is available.            |
| USB shell and data pull    | Present  | Supported     | Interface 0 enumerates at High-Speed; shell, pull and physical replug were exercised. |
| USB upload                 | Present  | Unqualified   | Upload/write behavior has not been exercised on this profile.                         |
| Host keyboard bridge       | N/A      | Unqualified   | Interface 1 exists, but host-keyboard forwarding has not been exercised.              |
| USB host mode              | Unknown  | Not supported | The target provides USB peripheral mode only.                                         |
| Removable storage          | Present  | Supported     | Insert before boot; FAT32 read/write works, but hot-swap does not.                    |
| Internal phone storage     | Present  | Not supported | Linux deliberately does not expose it.                                                |
| Audio                      | Present  | Not supported | No speaker, headphone or microphone support is provided.                              |
| Modem and mobile service   | Present  | Not supported | Calls, SMS and mobile data are unavailable.                                           |
| Bluetooth                  | Unknown  | Not supported | No Bluetooth support is provided.                                                     |
| Wi-Fi                      | Absent   | N/A           | This board variant has no Wi-Fi controller.                                           |
| Camera                     | Present  | Not supported | No camera pipeline or sensor driver is provided.                                      |
| Battery and charging       | Present  | Not supported | No battery reporting or charge control is provided.                                   |
| Indicator LEDs / vibration | Unknown  | Not supported | No indicator or vibration control is provided.                                        |
| Power-off                  | N/A      | Supported     | Battery-only shutdown works when charger power is absent.                             |
| Reboot and suspend         | N/A      | Not supported | Neither path is provided.                                                             |

## Build and start from source

Follow [Building FPLinux](../../docs/BUILDING.md) for host requirements and
source checks. Build the target from the repository root:

```sh
./fplinux build nokia-ta1618
```

For every RAM run, use this order:

1. Power the phone off and disconnect USB.
2. Start the loader:

    ```sh
    ./fplinux run nokia-ta1618
    ```

3. Wait until it explicitly asks for the phone.
4. Only then hold `*` and connect the powered-off phone, keeping `*` held as
   instructed.

The loader starts a volatile RAM session and does not change the vendor firmware
or internal storage. If the phone was connected early, disconnect it and restart
the sequence.

## Target-specific use

The cold-owned USB profile supports the shell and host data pull on interface 0.
USB upload, host-keyboard forwarding and application install/use remain
unqualified. The [console contract](../../docs/porting/CONSOLE.md) and
[file-transfer guide](../../docs/TRANSFER.md) describe the shared behavior of
those interfaces.

The [application guide](../../docs/APPLICATIONS.md) covers the shared APK
upload, install, run, and removal workflow for this source checkout.

### TyrQuake

TyrQuake 0.71 requires legally obtained game data. Insert a microSD card before
boot and put `pak0.pak` at:

```text
/mnt/card/fplinux/quake/id1/pak0.pak
```

In the application guide, select either phone or keyboard input. Phone mode uses
the physical keypad; keyboard mode uses the forwarded host keyboard. Hold the
phone counter-clockwise with the display on the left and the keypad on the right:

| Key                    | Menu                 | Game                        |
| ---------------------- | -------------------- | --------------------------- |
| D-pad `UP` / `DOWN`    | Left / right         | Turn left / right           |
| D-pad `LEFT` / `RIGHT` | Down / up            | Walk backward / forward     |
| Centre or dial         | Select               | Fire                        |
| Right soft             | Back                 | Menu                        |
| Left soft or `*`       | —                    | Jump                        |
| `0`                    | —                    | Fire                        |
| `1` / `3`              | —                    | Strafe left / right         |
| `2` / `5`              | —                    | Turn left / right           |
| `4` / `6`              | —                    | Walk backward / forward     |
| `7` / `9`              | —                    | Previous / next weapon      |
| `8`                    | —                    | Run while held              |
| `#`                    | Available for a bind | Available for a custom bind |

The launcher uses temporary runtime storage. TyrQuake settings and saves are
discarded when it exits; they are not written to microSD.

### MicroPythonOS

MicroPythonOS requires both the base and TA-1618 companion packages. Use the
[Nokia companion workflow](../../docs/APPLICATIONS.md#nokia-ta-1618-companion).

With a usable FAT32 card inserted before boot, MicroPythonOS uses `/mnt/card` and
keeps application state under `/mnt/card/.fplinux/micropythonos`. It mounts the
card when no matching mount exists and unmounts only a card that it mounted when
the interface exits. Without a usable card, application state remains in RAM.
The packages provide only features backed by the currently supported screen,
keypad and filesystem interfaces; unsupported hardware services are not shown.
The local launcher, keypad text input, File Manager, card-backed state, synced
write, read after remount and clean unmount have been exercised on this phone.
The USB-dependent installation path is hardware-unqualified for the current
cold-owned profile. Individual bundled applications outside those paths are not
qualified.

## microSD card

Insert the card before Linux starts. Use the first partition when present, or
the whole card otherwise:

```sh
card=/dev/mmcblk0p1
[ -b "$card" ] || card=/dev/mmcblk0
mkdir -p /mnt/card
mount -t vfat -o rw "$card" /mnt/card
```

For game data, a read-only mount is appropriate:

```sh
mount -t vfat -o ro,nodev,nosuid,noexec,utf8=1 "$card" /mnt/card
```

Flush and unmount before removing the card or ending the RAM session:

```sh
sync
umount /mnt/card
```

Do not remove a mounted card. Hot-swap is not supported. The TyrQuake launcher
uses a read-only mount when `/mnt/card` is not already mounted and otherwise
keeps the existing mount options.

## Keypad backlight

The binary keypad backlight control is available from the phone shell:

```sh
echo 1 > /sys/class/leds/:kbd_backlight/brightness
echo 0 > /sys/class/leds/:kbd_backlight/brightness
```

## End the RAM session

To end the RAM session, detach with `Ctrl-]`, disconnect USB, make sure charger
power is absent, then hold the red handset key continuously for five seconds. A
short press is an ordinary input event and releasing the key early cancels the
request. A successful shutdown discards the RAM session; boot normally to return
to the vendor firmware. If the phone remains powered after shutdown starts,
remove and reinsert the battery before booting. Linux reboot is not supported.

## Release status

There is no prebuilt archive or qualified executable payload for this target. A
local `./fplinux package nokia-ta1618 --candidate` archive is a hardware
qualification candidate, not a release. See [Release archives](../../docs/RELEASES.md).
