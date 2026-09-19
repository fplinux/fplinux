# Bluetooth

FPLinux provides Bluetooth Classic BR/EDR on targets that name it as supported.
The default RAM and `microsd-uboot` profiles use the same Bluetooth interfaces.
With prepared firmware, the controller, D-Bus, Bluetooth and OBEX services
start after the system root is mounted; no manual service start is required.
Without firmware, the system can boot for preparation but has no Bluetooth
controller.

Keep the USB-NCM connection available for local management while using
Bluetooth. It is independent of Bluetooth traffic.

Firmware must be prepared from the exact phone before building its image.
Preparation requires a source checkout; a standalone archive's runner does not
provide firmware preparation. Use an image built with the inputs fitted to that
phone, not firmware copied from another device.

`fplinux-bluetooth -h` or `fplinux-bluetooth --help` lists the available
commands. Place `-h` or `--help` after `enable`, `send`, `receive`, or `network`
for that command's syntax. Help is shown without contacting D-Bus or changing
the controller state.

## Pair and trust a peer

Use `bluetoothctl` on both peers and identify each controller by its exact MAC
address from `show`. On the phone, open a short pairing window:

```text
bluetoothctl --agent NoInputNoOutput
default-agent
power on
show
pairable on
discoverable-timeout 120
discoverable on
```

On the other peer, start a session:

```text
bluetoothctl --agent NoInputNoOutput
default-agent
power on
show
```

After every new FPLinux RAM boot, enter `remove <PHONE-MAC>` on this peer
if the old phone record is still listed. The phone's link key was lost with the
previous RAM session, so that record cannot be reused. Discover and pair with
the phone, then trust only its exact address:

```text
pairable on
scan bredr
devices
pair <PHONE-MAC>
trust <PHONE-MAC>
scan off
pairable off
```

Back on the phone, trust only the peer's exact address and close the pairing
window while keeping incoming connections available:

```text
trust <PEER-MAC>
discoverable off
pairable off
mgmt.connectable on
```

Send each command separately and wait for it to finish before sending the next.
On the phone, disabling discoverability also clears connectability, so run
`mgmt.connectable on` after both `off` commands when the phone must accept an
incoming OPP connection.

`NoInputNoOutput` uses Just Works pairing and provides no passkey comparison.
Verify both MAC addresses separately, pair only with the intended nearby peer,
and keep the pairable and discoverable window short.

## Receive a file on the phone

Create the destination directory, then open a one-file receive window for the
exact paired peer:

```sh
mkdir -p /tmp/bluetooth-receive
fplinux-bluetooth receive <PEER-MAC> /tmp/bluetooth-receive 120
```

The directory must already exist. The last argument is a timeout from 1 through
3600 seconds. While the command runs, send one file from the named peer with a
compatible RFCOMM OPP client. The window rejects other peers and closes after
one transfer or the timeout. It never overwrites an existing destination
filename.

## Send a file from the phone

First start a compatible RFCOMM OPP receiver on the paired peer, then run:

```sh
fplinux-bluetooth send <PEER-MAC> /tmp/example.bin
```

The source must be a readable regular file. Compatibility depends on the
peer's RFCOMM OPP service; arbitrary stock desktop Bluetooth and OBEX setups
are not covered by this support claim.

## Use a PAN Internet connection

Before pairing, configure the other peer to advertise a Bluetooth NAP service.
It must provide DHCP, DNS, Internet routing, and any required forwarding or
NAT; FPLinux does not configure the peer.

In the first phone shell, connect to the exact peer and keep the command in the
foreground:

```sh
fplinux-bluetooth network <PEER-MAC>
```

The command reports the created interface, for example `bnep0`, and owns the
BlueZ D-Bus connection for the lifetime of PAN. In a second phone shell, use
the reported interface rather than assuming its number:

```sh
udhcpc -i bnep0
```

Keep the first command running while using the connection. Stop it with
Ctrl-C to disconnect PAN. Internet traffic uses the configured BNEP interface;
USB-NCM remains the separate local management path and is not the Internet
gateway.

## Suspend

The shared Bluetooth driver supports s2idle without discarding pairing state.
The selected target's documentation states which profiles and storage
conditions are supported on that phone. On a supported configuration, HCI,
OPP and PAN can be used after wake without restarting services or pairing again.

Use the target's suspend instructions for its controls and wake sources.
In a standalone archive, the top-level `README.txt` identifies those pages.

Finish OPP transfers and stop the foreground PAN command before sleeping.
Wait until the peer is disconnected. A connected link or busy controller
causes the sleep request to fail rather than tearing down the link. Use the
phone's supported key or RTC alarm to wake it; Bluetooth wake is not qualified.

## Limits and persistence

- Pairing records, keys, and files left in RAM disappear after power loss or a
  fresh RAM load. The microSD system root stores pairing records persistently;
  temporary RAM files still disappear. Save needed files to persistent storage.
- Bluetooth use does not write the phone's NAND or NV storage.
- Warm controller restart, BLE, Bluetooth audio, HID, range, wake over
  Bluetooth and deep suspend are not qualified.
