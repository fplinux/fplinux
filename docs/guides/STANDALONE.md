# Using a standalone archive

A standalone archive contains one target-specific FPLinux image, its host
runner, installable applications and the documentation needed to use them
without a source checkout. Read the top-level `README.txt` first: it names the
phone, its boot key, supported hardware, storage limits and safe way to end the
RAM session.

If `CANDIDATE-NOTICE.txt` is present, this is a phone-test candidate: its
executable payload has not yet been phone-tested. Creating or checking the
archive does not turn that candidate into a release.

## Host requirements

The host needs:

- Linux x86-64;
- Python 3.14;
- GNU `stdbuf` from coreutils;
- `ip` from iproute2;
- `ssh`, `ssh-keygen`, `ssh-keyscan` and `sftp` from OpenSSH;
- a network manager that runs IPv4 DHCP on a new USB-NCM interface;
- permission to access USB devices `1782:4d00` and `0525:a4a6`.

NetworkManager is supported. The archive contains its loader and other
target-specific host tools.

## Prepare the host

Extract the complete top-level directory, enter it and verify every archived
file before connecting the phone:

```sh
cd <extracted-top-level-directory>
sha256sum -c SHA256SUMS
```

Install the bundled udev rules, reload them, then disconnect the phone if it is
already attached:

```sh
sudo install -m 0644 ./60-fplinux.rules /etc/udev/rules.d/60-fplinux.rules
sudo udevadm control --reload-rules
```

The bundled rules use desktop logind access. On a headless host, adapt the local
copy to a trusted group with `MODE="0660"`. Never make either USB device
world-writable. Run the loader as the regular user; host-keyboard forwarding is
the only documented operation that normally needs elevated access.

## Start the session

If the archive contains `FPLINUX.img.xz`, prepare and insert the system card
using [microSD system root](MICROSD_ROOT.md) before loading. An archive without
that image uses a volatile RAM root.

Power the phone off and disconnect USB. Start the runner before attaching the
phone:

```sh
./runner/run.py
```

Wait until it explicitly requests the device. Only then hold the boot key from
the top-level `README.txt` and connect the powered-off phone, keeping the key
held as instructed. If the phone was connected too early, disconnect it and
restart this sequence.

The loader writes only volatile RAM. It does not flash, erase, partition or
write internal phone storage. Any removable-media writes are separate and must
follow the selected target's instructions.

From an interactive terminal, the runner opens the new SSH session. Without an
input terminal, it returns successfully when that session is ready. Reconnect
with the command below to open a shell later.

## Use the running session

Exiting the shell does not stop Linux. Disconnecting USB leaves Linux running
only while another power source is available; without a battery, it cuts
power. Reopen a shell on a still-running phone with:

```sh
./runner/run.py --reconnect
```

For details, use the bundled pages:

### Access and file transfer

- [USB networking](../features/USB_NETWORKING.md);
- [SSH access](../features/SSH.md);
- [file transfer](../features/FILE_TRANSFER.md);
- [Bluetooth](../features/BLUETOOTH.md), on supported targets with prepared firmware;

### Local interface

- [local console](../features/LOCAL_CONSOLE.md);
- [host keyboard forwarding](../features/HOST_KEYBOARD.md);
- [headphone audio](../features/HEADPHONE_AUDIO.md), on supported phones;
- [speaker audio](../features/SPEAKER_AUDIO.md), on supported phones;
- [phone microphone](../features/MICROPHONE_AUDIO.md), on supported phones;
- [FM radio](../features/FM_RADIO.md), on supported phones with fitted settings;
- [LCD backlight](../features/DISPLAY_BACKLIGHT.md) and
  [keypad backlight](../features/KEYPAD_BACKLIGHT.md);
- [vibration](../features/VIBRATION.md), subject to the phone's physical limits;

### Storage and power

- [removable microSD storage](../features/MICROSD.md), including safe removal;
- [microSD system root](MICROSD_ROOT.md);
- [real-time clock](../features/RTC.md);
- [power-off](../features/POWER_OFF.md);
- [suspend](../features/SUSPEND.md);

### Hardware telemetry

- [CPU clock and frequency selection](../features/CPU_CLOCK.md);
- [charger status](../features/CHARGER_STATUS.md),
  [battery telemetry](../features/BATTERY_TELEMETRY.md),
  [SoC temperature](../features/SOC_TEMPERATURE.md) and
  [auxiliary ADC](../features/AUXADC.md);

### Applications and packages

- [installing and removing optional APK packages](APK_PACKAGES.md);
- [FPLinux: ARMADA](../apps/SHOWCASE.md);
- [TyrQuake](../apps/TYRQUAKE.md);
- [MicroPythonOS](../apps/MICROPYTHONOS.md);
- [image rotation](../apps/ROTATE.md);
- [JPEG codec and scaling](../apps/JPEG.md);
- [native image presentation](../apps/PRESENT.md).

## End the session

In a RAM-only session, flush and unmount removable filesystems first. With a
microSD system root, follow [system-card shutdown](MICROSD_ROOT.md#persistence-and-shutdown)
before cutting power. Use the exact shutdown procedure in the top-level
`README.txt`. Closing SSH leaves Linux running; unplugging USB from a phone
without a battery removes power immediately.
