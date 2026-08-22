# INOI 240 Modern 4G

## Device

| Field    | Value                                                      |
| -------- | ---------------------------------------------------------- |
| Device   | INOI 240 Modern 4G                                         |
| Platform | [Unisoc UMS9117 / T117](../../platforms/ums9117/README.md) |
| Profile  | `console`                                                  |
| Boot     | Volatile RAM only                                          |

## Status

This experimental target provides a local framebuffer terminal, physical keypad,
USB shell and host-keyboard bridge on an INOI 240 Modern 4G. The session is lost
when power is removed and does not access internal phone storage.

## Evidence basis

Supported entries record physical development observations on this exact phone:
the terminal, keypad, USB interfaces, host-keyboard bridge, TyrQuake, and
MicroPythonOS installation, launcher navigation, and keypad text input have
been exercised. The `128×160` MicroPythonOS UI is only partially adaptive;
launcher content and some application controls do not fully fit. These
observations are not a release qualification record and do not identify a
qualified candidate or executable payload.

## Hardware support

**Hardware** records only what is established for this phone; **Unknown** does
not mean absent. **FPLinux** is **Supported** only after exercise on this exact
variant. **Not supported** describes the current target, not the hardware.

| Area                        | Hardware | FPLinux       | What a user can rely on / limit                                |
| --------------------------- | -------- | ------------- | -------------------------------------------------------------- |
| RAM boot                    | N/A      | Supported     | Linux runs only in RAM; it is discarded when the session ends. |
| Persistent boot             | N/A      | Not supported | No autonomous Linux boot path is provided.                     |
| Local screen                | Present  | Supported     | `128×160` framebuffer console.                                 |
| Physical keypad             | Present  | Supported     | Local terminal, TyrQuake and MicroPythonOS input.              |
| Keypad backlight            | Unknown  | Not supported | No FPLinux keypad-backlight control is provided.               |
| USB shell and file transfer | Present  | Supported     | Interface 0 provides the shell and transfer commands.          |
| Host keyboard bridge        | N/A      | Supported     | Interface 1 forwards one host evdev keyboard.                  |
| USB host mode               | Unknown  | Not supported | The target provides USB peripheral mode only.                  |
| Removable storage           | Unknown  | Not supported | No microSD driver is provided.                                 |
| Internal phone storage      | Present  | Not supported | Linux deliberately does not expose it.                         |
| Audio                       | Present  | Not supported | No speaker, headphone or microphone support is provided.       |
| Modem and mobile service    | Present  | Not supported | Calls, SMS and mobile data are unavailable.                    |
| Bluetooth                   | Unknown  | Not supported | No Bluetooth support is provided.                              |
| Wi-Fi                       | Unknown  | Not supported | No Wi-Fi support is provided.                                  |
| Camera                      | Unknown  | Not supported | No camera support is provided.                                 |
| Battery and charging        | Present  | Not supported | No battery reporting or charge control is provided.            |
| Indicator LEDs / vibration  | Unknown  | Not supported | No indicator or vibration control is provided.                 |
| Power-off                   | N/A      | Not supported | Disconnect USB, reseat the battery, then boot normally.        |
| Reboot and suspend          | N/A      | Not supported | Neither path is provided.                                      |

## Build and start from source

Follow [Building FPLinux](../../docs/BUILDING.md) for host requirements and
source checks. Build the target from the repository root:

```sh
./fplinux build inoi-240-modern-4g
```

For every RAM run, use this order:

1. Power the phone off and disconnect USB.
2. Start the loader:

    ```sh
    ./fplinux run inoi-240-modern-4g
    ```

3. Wait until it explicitly asks for the phone.
4. Only then hold `*` and connect the powered-off phone, keeping `*` held as
   instructed.

The loader writes the volatile RAM session only. If the phone was connected
early, disconnect it and restart the sequence.

## Target-specific use

### TyrQuake

Use the shared [application guide](../../docs/APPLICATIONS.md) to install,
start, and remove the APK. This FPLinux target has no supported microSD path,
so a RAM-only session can put a legally obtained `pak0.pak` in tmpfs:

```sh
./fplinux console inoi-240-modern-4g \
  --exec 'mkdir -p /mnt/card && mount -t tmpfs tmpfs /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
./fplinux console inoi-240-modern-4g \
  --upload ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak
```

`--input phone` uses the physical keypad; `--input keyboard` uses the forwarded
host keyboard. The phone has 64 MiB of RAM and the game reserves 32 MiB; keeping
a full PAK in tmpfs leaves little memory and there is no swap.

### MicroPythonOS

Use the shared [application guide](../../docs/APPLICATIONS.md). The package uses
the shared FPLinux display and keypad ABI. Its `128×160` UI remains partial:
launcher content and some application controls are clipped. Individual bundled
applications outside the exercised paths are not qualified. This FPLinux target
has no supported microSD path, so application state remains in RAM.

## End the RAM session

Disconnect USB, remove and reinsert the battery, then boot the phone normally.
This target does not provide Linux power-off or reboot.

## Release status

There is no prebuilt archive or qualified executable payload for this target. A
local `./fplinux package inoi-240-modern-4g --candidate` archive is a hardware
qualification candidate, not a release. See [Release archives](../../docs/RELEASES.md).
