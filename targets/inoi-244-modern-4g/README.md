# INOI 244 Modern 4G

## Device

| Field    | Value                                                      |
| -------- | ---------------------------------------------------------- |
| Device   | INOI 244 Modern 4G (`F2444G`)                              |
| Platform | [Unisoc UMS9117 / T117](../../platforms/ums9117/README.md) |
| Profile  | `console`                                                  |
| Boot     | Volatile RAM only                                          |

## Status

This experimental target provides a local framebuffer terminal, physical keypad,
USB shell and host-keyboard bridge on an INOI 244 Modern 4G. These functions and
TyrQuake have been exercised on the phone. MicroPythonOS installation, launcher
navigation and keypad text input have also been exercised. The session is lost
when power is removed; it does not access internal phone storage. This is
feature-level hardware evidence, not release qualification.

## Hardware support

**Hardware** records only what is established for this phone; **Unknown** does
not mean absent. **FPLinux** is **Supported** only after exercise on this exact
variant. **Not supported** describes the current target, not the hardware.

| Area                        | Hardware | FPLinux       | What a user can rely on / limit                                |
| --------------------------- | -------- | ------------- | -------------------------------------------------------------- |
| RAM boot                    | N/A      | Supported     | Linux runs only in RAM; it is discarded when the session ends. |
| Persistent boot             | N/A      | Not supported | No autonomous Linux boot path is provided.                     |
| Local screen                | Present  | Supported     | `240×320` framebuffer console.                                 |
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
./fplinux build inoi-244-modern-4g
```

For every RAM run, use this order:

1. Power the phone off and disconnect USB.
2. Start the loader:

    ```sh
    ./fplinux run inoi-244-modern-4g
    ```

3. Wait until it explicitly asks for the phone.
4. Only then hold `*` and connect the powered-off phone, keeping `*` held as
   instructed.

The loader writes the volatile RAM session only. If the phone was connected
early, disconnect it and restart the sequence.

## Use after boot

The local terminal is available on the phone; interface 0 is the USB shell and
transfer channel. Detach the host client with `Ctrl-]` without stopping Linux,
then reconnect or compare the running kernel with the local build:

```sh
./fplinux verify inoi-244-modern-4g
./fplinux console inoi-244-modern-4g
sudo ./fplinux console inoi-244-modern-4g --keyboard /dev/input/eventN
```

The keyboard forwarder uses interface 1 and keeps the selected host keyboard
away from the host desktop while it runs. See the [console contract](../../docs/porting/CONSOLE.md)
and [file-transfer guide](../../docs/TRANSFER.md) for shared behavior.

## Installable applications

### TyrQuake

TyrQuake 0.71 is provided as `fplinux-tyrquake.apk` under `apks/` in the
`output:` directory printed by `./fplinux build`; it is not installed in the
standard root filesystem. Set `bundle` to that printed directory, then install
it in the current RAM session:

```sh
./fplinux console inoi-244-modern-4g --upload \
  "$bundle/apks/fplinux-tyrquake.apk" /tmp/fplinux-tyrquake.apk
./fplinux console inoi-244-modern-4g --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-tyrquake.apk'
```

This target has no microSD driver, so a RAM-only session can put a legally
obtained `pak0.pak` in tmpfs:

```sh
./fplinux console inoi-244-modern-4g \
  --exec 'mkdir -p /mnt/card && mount -t tmpfs tmpfs /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
./fplinux console inoi-244-modern-4g \
  --upload ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak
./fplinux console inoi-244-modern-4g --exec 'quake --input phone'
./fplinux console inoi-244-modern-4g --exec 'quake --input keyboard'
```

`--input phone` uses the physical keypad; `--input keyboard` uses the forwarded
host keyboard. The phone has 64 MiB of RAM and the game reserves 32 MiB; keeping
a full PAK in tmpfs leaves little memory and there is no swap. After exiting the
game, remove it without restarting the phone:

```sh
./fplinux console inoi-244-modern-4g --exec 'apk del fplinux-tyrquake'
```

### MicroPythonOS

MicroPythonOS is `fplinux-micropythonos.apk`. The package targets the shared
FPLinux display and keypad ABI. Installation and removal, the `240×320` launcher,
keypad text input and terminal restoration have been exercised on this phone.
Individual bundled applications outside the exercised paths are not qualified.
This target has no microSD capability, so application state remains in RAM.

```sh
./fplinux console inoi-244-modern-4g --upload \
  "$bundle/apks/fplinux-micropythonos.apk" /tmp/fplinux-micropythonos.apk
./fplinux console inoi-244-modern-4g --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-micropythonos.apk'
./fplinux console inoi-244-modern-4g
```

At the phone shell prompt, run `micropythonos`. Press `Ctrl-C` to stop it and
restore the terminal, then `Ctrl-]` to detach before removing the package:

```sh
./fplinux console inoi-244-modern-4g --exec 'apk del fplinux-micropythonos'
```

## End the RAM session

Disconnect USB, remove and reinsert the battery, then boot the phone normally.
This target does not provide Linux power-off or reboot.

## Release status

There is no prebuilt archive or qualified runtime closure for this target. A
local `./fplinux package inoi-244-modern-4g --candidate` archive is a hardware
qualification candidate, not a release. See [Release archives](../../docs/RELEASES.md).
