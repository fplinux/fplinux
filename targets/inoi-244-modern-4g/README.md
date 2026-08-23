# INOI 244 Modern 4G

## Device

| Field    | Value                                                      |
| -------- | ---------------------------------------------------------- |
| Device   | INOI 244 Modern 4G (`F2444G`)                              |
| Platform | [Unisoc UMS9117 / T117](../../platforms/ums9117/README.md) |
| Boot     | Volatile RAM only                                          |

## Status

This experimental target runs Linux in volatile RAM with a local `240×320`
terminal, physical keypad, USB SSH/SFTP and host-keyboard forwarding. Internal
phone storage remains inaccessible.

## Evidence basis

The **Supported** entries below were exercised on this phone. This is not a
release qualification.

## Hardware support

**Hardware** records only what is established for this phone; **Unknown** does
not mean absent. **FPLinux** is **Supported** only after exercise on this exact
variant. **Not supported** describes the current target, not the hardware.

| Area                        | Hardware | FPLinux       | What a user can rely on / limit                                           |
| --------------------------- | -------- | ------------- | ------------------------------------------------------------------------- |
| RAM boot                    | N/A      | Supported     | Linux runs only in RAM; it is discarded when the session ends.            |
| Persistent boot             | N/A      | Not supported | No autonomous Linux boot path is provided.                                |
| Local screen                | Present  | Supported     | `240×320` framebuffer console.                                            |
| Physical keypad             | Present  | Supported     | Local terminal, TyrQuake and MicroPythonOS input.                         |
| Keypad backlight            | Unknown  | Not supported | No FPLinux keypad-backlight control is provided.                          |
| USB networking              | Present  | Supported     | Private host link only; no gateway, DNS or forwarding.                    |
| SSH, SFTP and file transfer | N/A      | Supported     | Shell, commands and verified file transfer; physical replug is supported. |
| Host keyboard bridge        | N/A      | Supported     | Forwards one host evdev keyboard while the client runs.                   |
| USB host mode               | Unknown  | Not supported | The target provides USB peripheral mode only.                             |
| Removable storage           | Unknown  | Not supported | No supported microSD path is provided.                                    |
| Internal phone storage      | Present  | Not supported | Linux deliberately does not expose it.                                    |
| Audio                       | Present  | Not supported | No speaker, headphone or microphone support is provided.                  |
| Modem and mobile service    | Present  | Not supported | Calls, SMS and mobile data are unavailable.                               |
| Bluetooth                   | Unknown  | Not supported | No Bluetooth support is provided.                                         |
| Wi-Fi                       | Unknown  | Not supported | No Wi-Fi support is provided.                                             |
| Camera                      | Unknown  | Not supported | No camera support is provided.                                            |
| Battery and charging        | Present  | Not supported | No battery reporting or charge control is provided.                       |
| Indicator LEDs / vibration  | Unknown  | Not supported | No indicator or vibration control is provided.                            |
| Power-off                   | N/A      | Not supported | Disconnect USB, reseat the battery, then boot normally.                   |
| Reboot and suspend          | N/A      | Not supported | Neither path is provided.                                                 |

## Build and start from source

Use the shared [build and loader procedure](../../docs/BUILDING.md). When the
loader requests this phone, hold `*` and connect it powered off.

## Target-specific use

### TyrQuake

Use the shared [application guide](../../docs/APPLICATIONS.md) to install,
start, and remove the APK. This FPLinux target has no supported microSD path,
so a RAM-only session can put a legally obtained `pak0.pak` in tmpfs:

```sh
./fplinux console inoi-244-modern-4g \
  --exec 'mkdir -p /mnt/card && mount -t tmpfs tmpfs /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
./fplinux console inoi-244-modern-4g \
  --upload ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak
```

`--input phone` uses the physical keypad; `--input keyboard` uses the forwarded
host keyboard. The phone has 64 MiB of RAM and the game reserves 32 MiB; keeping
a full PAK in tmpfs leaves little memory and there is no swap.

### MicroPythonOS

Use the shared [application guide](../../docs/APPLICATIONS.md). This target has
no supported microSD path, so application state remains in RAM.

## End the RAM session

Disconnect USB, remove and reinsert the battery, then boot the phone normally.
This target does not provide Linux power-off or reboot.
