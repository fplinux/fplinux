# Release archives

FPLinux archives are target-specific Linux x86-64 bundles made from a successful
local build. They use the same RAM-only loader path as a source checkout.

## Current availability

This checkout has no prebuilt release archive and no recorded qualified payload.
A candidate is for physical qualification only and must not be published as a
release.

## Create a candidate

Build the exact target, then package it:

```sh
./fplinux build <target>
./fplinux package <target> --candidate
```

The candidate ZIP is written below `.cache/out/candidates/`. Its filename and
included notice identify it as a qualification candidate. Packaging validates the
selected build and does not rebuild it.

A release archive can be created only after the exact executable payload from a
candidate has completed phone-specific qualification:

```sh
./fplinux package <target>
```

Until then, this command refuses to create a release archive. A successful
release archive is written below `.cache/out/releases/`.

## Qualification boundary

A candidate proves that the source checkout produced a packageable bundle. It
does not prove that the target boots or that any hardware feature works.
Qualification covers the RAM runtime and bundled APKs. Documentation, notices,
checksums and build metadata remain outside that phone-qualified payload but are
still covered by archive integrity checks.

Changing the executable payload requires another phone qualification. A build,
archive checksum or host-side `verify` does not replace that phone test.

The [target index](../targets/README.md) links every phone's status, connection
instructions, supported controls, and shutdown or recovery procedure.

## Use an archive

Archive users need:

- Linux x86-64
- Python 3.11 or newer
- GNU `stdbuf` from coreutils
- USB access for the loader and running session
- iproute2 and the OpenSSH client tools (`ssh`, `ssh-keygen`, `ssh-keyscan`, and
  `sftp`)
- a host network manager that automatically runs IPv4 DHCP on a newly attached
  USB-NCM interface; NetworkManager is supported

Extract the whole archive, enter its top-level directory, and verify it before
connecting a phone:

```sh
cd <extracted-top-level-directory>
sha256sum -c SHA256SUMS
```

Then power the phone off and disconnect USB. Start the bundled runner first:

```sh
./runner/run.py
```

Wait until it requests the device, then connect the phone and follow the boot-key
instructions supplied by the matching target document. The runner loads volatile
RAM only. Reconnect without starting another RAM load:

```sh
./runner/run.py --reconnect
```

Archives contain the RAM image, required host tools and assets, bundled
installable APKs, a runner, checksums, target instructions, and license notices.
They do not contain build trees, caches, or kernel debug output.

## USB access

Current targets use the Unisoc BootROM device `1782:4d00` during the
RAM-load sequence and Linux gadget `0525:a4a6` after boot. On a desktop system
managed by logind, create a local udev rules file such as
`/etc/udev/rules.d/70-fplinux.rules`:

```udev
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="1782", ATTR{idProduct}=="4d00", TAG+="uaccess"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0525", ATTR{idProduct}=="a4a6", TAG+="uaccess"
```

For a headless host, use a distribution-appropriate local group and
`MODE="0660"` instead of `TAG+="uaccess"`. Do not make the devices
world-writable. Reload rules, disconnect the phone, and reconnect it:

```sh
sudo udevadm control --reload-rules
```

Run the loader as the regular user. Use elevated privileges only where the
matching target documentation requires them, such as forwarding a host keyboard.
The Linux gadget ID `0525:a4a6` is development-only and cannot identify a
release.

## Console lifecycle

See [Host-to-phone transfer](TRANSFER.md) for shell and file-transfer behavior.
Target documents define phone-specific power-off and recovery.

## Troubleshooting

### The runner waits for a device

Power the phone off, disconnect USB, and restart the runner. Do not connect the
phone until it requests the device. Follow the selected target's boot-key
instructions exactly.

### USB is visible but access is denied

Install the rules in [USB access](#usb-access), reload them, and reconnect the
phone. Loader and running-session USB permissions are separate.

### The phone is already running FPLinux

Reconnect with `./runner/run.py --reconnect`. Do not start a new RAM load unless
beginning again from a powered-off phone.

### The loader or console does not complete

Verify the extracted archive with `sha256sum -c SHA256SUMS` and retain the
complete runner output for diagnosis.
