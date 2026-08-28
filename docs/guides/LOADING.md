# Loading from a source checkout

This guide covers starting FPLinux from a source checkout in volatile RAM.
Before connecting a phone, select the exact target from the
[target index](../../targets/README.md) and read its boot key, current hardware
support, storage rules, and shutdown or recovery method. For a packaged image,
use [Using a standalone archive](STANDALONE.md) instead.

## Runtime requirements

The current default targets require:

- Linux x86-64;
- Python 3.11 or newer;
- GNU `stdbuf` from coreutils;
- `ip` from iproute2;
- the OpenSSH client tools `ssh`, `ssh-keygen`, `ssh-keyscan`, and `sftp`;
- a host network manager that automatically runs IPv4 DHCP on a newly attached
  USB-NCM interface; NetworkManager is supported;
- read and write access to the loader and Linux USB devices described below.

Source builds additionally require the tools in
[Building FPLinux](BUILDING.md#requirements-and-setup).

## USB access

Current targets use the Unisoc BootROM device `1782:4d00` during the RAM load
and Linux gadget `0525:a4a6` after boot. On a desktop managed by logind, create
`/etc/udev/rules.d/70-fplinux.rules` with:

```udev
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="1782", ATTR{idProduct}=="4d00", TAG+="uaccess"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0525", ATTR{idProduct}=="a4a6", TAG+="uaccess"
```

For a headless host, use a distribution-appropriate local group and
`MODE="0660"` instead of `TAG+="uaccess"`. Do not make either device
world-writable. Reload the rules, disconnect the phone, and reconnect it:

```sh
sudo udevadm control --reload-rules
```

Run the loader as the regular user. Use elevated privileges only for an action
that requires them, such as host-keyboard forwarding. The Linux gadget ID is a
development interface and does not identify a target or release.

## Start a fresh RAM session

A fresh load always starts with the phone powered off and USB disconnected.
Start the loader for the already selected target first. USB enumeration is
diagnostic evidence; it never selects a target.

### From a source checkout

Build the target as described in [Building FPLinux](BUILDING.md), then run:

```sh
./fplinux run <target>
```

For a runnable development profile, use the same profile that selected the
build. Build-only profiles are rejected before the loader touches USB.

```sh
./fplinux run <target> --profile <profile>
```

The Nokia microSD system mode has a public boot selector. Its build remains
isolated from the ordinary RAM-only bundle:

```sh
./fplinux build nokia-ta1618 --profile microsd-uboot
./fplinux run nokia-ta1618 --boot microsd
```

The second command selects only that prepared microSD context. It does not fall
back to the default Nokia bundle. Running `./fplinux run nokia-ta1618` without
`--boot` continues to use the ordinary RAM-only system.

A profile with `transport = "none"` returns after the bridge acknowledges the
session-bound handoff and the original BootROM USB device disappears. With no
Linux-side transport, establish startup separately on the phone.

### Connect the phone

Wait until the selected loader explicitly requests the device. Only then hold
the boot key named by the target document and connect the powered-off phone,
keeping the key held as instructed. If the phone was connected too early,
disconnect it and restart the loader-first sequence.

The supported loader path writes only volatile RAM. It does not flash, erase,
partition, or write internal phone storage. A target may separately support
writes to removable media through its documented mounted-filesystem workflow.

## After boot

Exiting the shell or unplugging USB does not end Linux. Do not start another RAM
load merely to reopen the shell. Use [USB networking](../features/USB_NETWORKING.md)
to understand the private host link and [SSH access](../features/SSH.md) to open
or reconnect to the running session. [File transfer](../features/FILE_TRANSFER.md)
and [host keyboard forwarding](../features/HOST_KEYBOARD.md) are separate
features of that session.

## Verify a source-checkout session

With the default target bundle loaded and its console ready, run:

```sh
./fplinux verify <target>
```

`verify` compares the running kernel identity with the selected local bundle
and refuses a stale local bundle. It proves only that relationship; it is not a
phone hardware-qualification test and does not apply to standalone archives.

## End the RAM session

Exiting SSH and disconnecting USB leave the volatile Linux session running.
Before ending it, flush and unmount any removable media according to the target
document. Then follow that document's
[target-specific end-of-session procedure](../../targets/README.md). Current
targets differ: some provide battery-only Linux power-off, while others require
removing and reinserting the battery. Linux reboot is not a supported substitute.

## Troubleshooting

### The loader waits for a device

Power the phone off, disconnect USB, and restart the loader. Do not connect the
phone until it requests the device. Follow the selected target's boot-key
instructions exactly.

### USB is visible but access is denied

Install the rules in [USB access](#usb-access), reload them, disconnect the
phone, and reconnect it. BootROM and running-session USB permissions are
separate.

### The phone is already running FPLinux

Use [SSH access](../features/SSH.md) to reconnect. Do not start a fresh load
unless beginning again from a powered-off phone.

### Linux USB appears but the console does not connect

Confirm that the host network manager assigned IPv4 configuration to the new
USB-NCM interface and that the required OpenSSH tools are installed. Preserve
the complete loader or reconnect output for diagnosis.

### Verification reports a different build

Rebuild the selected target and begin a fresh RAM session with that output.
`verify` deliberately refuses to equate a running image with stale local build
state.
