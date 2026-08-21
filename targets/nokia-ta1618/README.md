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
backlight, USB shell, host-keyboard bridge and microSD access on a TA-1618. Boot,
kernel, screen, keypad, backlight, USB interfaces, host keyboard bridge, microSD
and battery-only power-off have been exercised on the phone. The Linux session is
volatile and internal phone storage stays inaccessible. This is feature-level
hardware evidence, not release qualification.

## Hardware support

**Hardware** records only what is established for this phone; **Unknown** does
not mean absent. **FPLinux** is **Supported** only after exercise on this exact
variant. **Not supported** describes the current target, not the hardware.

| Area                        | Hardware | FPLinux       | What a user can rely on / limit                                            |
| --------------------------- | -------- | ------------- | -------------------------------------------------------------------------- |
| RAM boot                    | N/A      | Supported     | Linux runs only in RAM and does not write phone storage.                   |
| Persistent boot             | N/A      | Not supported | No autonomous Linux boot path is provided.                                 |
| Local screen                | Present  | Supported     | `240×320` framebuffer terminal.                                            |
| Physical keypad             | Present  | Supported     | Local terminal and TyrQuake input.                                         |
| Keypad backlight            | Present  | Supported     | A key press lights it for about five seconds; manual control is available. |
| USB shell and file transfer | Present  | Supported     | Interface 0 provides the shell and transfer commands.                      |
| Host keyboard bridge        | N/A      | Supported     | Interface 1 forwards one host evdev keyboard.                              |
| USB host mode               | Unknown  | Not supported | The target provides USB peripheral mode only.                              |
| Removable storage           | Present  | Supported     | Insert before boot; FAT32 read/write works, but hot-swap does not.         |
| Internal phone storage      | Present  | Not supported | Linux deliberately does not expose it.                                     |
| Audio                       | Present  | Not supported | No speaker, headphone or microphone support is provided.                   |
| Modem and mobile service    | Present  | Not supported | Calls, SMS and mobile data are unavailable.                                |
| Bluetooth                   | Unknown  | Not supported | No Bluetooth support is provided.                                          |
| Wi-Fi                       | Absent   | N/A           | This board variant has no Wi-Fi controller.                                |
| Camera                      | Present  | Not supported | No camera pipeline or sensor driver is provided.                           |
| Battery and charging        | Present  | Not supported | No battery reporting or charge control is provided.                        |
| Indicator LEDs / vibration  | Unknown  | Not supported | No indicator or vibration control is provided.                             |
| Power-off                   | N/A      | Supported     | Battery-only shutdown works when charger power is absent.                  |
| Reboot and suspend          | N/A      | Not supported | Neither path is provided.                                                  |

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

## Use after boot

The local terminal starts on the phone. Interface 0 is the USB shell and transfer
channel; detach a host client with `Ctrl-]` without stopping Linux. Reconnect or
compare the running kernel with the local build:

```sh
./fplinux verify nokia-ta1618
./fplinux console nokia-ta1618
sudo ./fplinux console nokia-ta1618 --keyboard /dev/input/eventN
```

The keyboard forwarder uses interface 1 and keeps the selected host keyboard
away from the host desktop while it runs. See the [console contract](../../docs/porting/CONSOLE.md)
and [file-transfer guide](../../docs/TRANSFER.md) for shared behavior.

## TyrQuake

The image includes TyrQuake 0.71, but not Quake game data. Insert a microSD card
before boot and put a legally obtained PAK at:

```text
/mnt/card/fplinux/quake/id1/pak0.pak
```

From the phone shell, start exactly one input mode:

```sh
quake --input phone
quake --input keyboard
```

Phone mode uses the physical keypad. Keyboard mode uses the forwarded host
keyboard. Hold the phone counter-clockwise with the display on the left and the
keypad on the right:

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

There is no prebuilt archive or qualified runtime closure for this target. A
local `./fplinux package nokia-ta1618 --candidate` archive is a hardware
qualification candidate, not a release. See [Release archives](../../docs/RELEASES.md).
