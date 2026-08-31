# Using a standalone archive

A standalone archive contains one target-specific FPLinux image, its host
runner, installable applications and the documentation needed to use them
without a source checkout. Read the top-level `README.txt` first: it names the
phone, its boot key, supported hardware, storage limits and safe way to end the
RAM session.

If `CANDIDATE-NOTICE.txt` is present, the executable payload is awaiting
physical qualification. Creating or checking the archive does not turn that
candidate into a release.

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

## Start the RAM session

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

Exiting the shell or unplugging USB does not stop Linux. Reconnect with:

```sh
./runner/run.py --reconnect
```

For details, use the bundled pages:

- [local console](../features/LOCAL_CONSOLE.md);
- [USB networking](../features/USB_NETWORKING.md);
- [SSH access](../features/SSH.md);
- [file transfer](../features/FILE_TRANSFER.md);
- [host keyboard forwarding](../features/HOST_KEYBOARD.md);
- [CPU clock reporting](../features/CPU_CLOCK.md);
- [TyrQuake](../apps/TYRQUAKE.md);
- [MicroPythonOS](../apps/MICROPYTHONOS.md).

## End the session

Flush and unmount any removable media first. Then use the exact shutdown or
recovery procedure in the top-level `README.txt`. Closing SSH or disconnecting
USB alone leaves the RAM session running.
