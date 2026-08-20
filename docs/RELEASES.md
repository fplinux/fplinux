# Release archives

FPLinux packages are phone- and host-specific. They are generated from the same
build outputs available through `./fplinux build`.

## Availability

The repository does not provide a prebuilt archive. `releases.lock.toml` contains
no qualified runtime closure for `nokia-ta1618`, and `.cache/out/releases/` is
not a source or release record.

The package target is Linux x86-64. Windows archives are not available.

## Qualification contract

Build the target, then create a candidate for the physical phone gate:

```sh
./fplinux build nokia-ta1618
./fplinux package nokia-ta1618 --candidate
```

The candidate is written to:

```text
.cache/out/candidates/FPLinux-Nokia-3210-4G-TA1618-console-candidate-linux-x86_64-<content16>.zip
```

Every candidate contains a generated `CANDIDATE-NOTICE.txt` whose first line is
`HARDWARE QUALIFICATION CANDIDATE - DO NOT PUBLISH`. The candidate notice is
included in `SHA256SUMS` and in the complete package-content digest. The package
command verifies that every generated bundle input matches the successful build
manifest and that the manifest belongs to the selected source workspace and
container recipe.

A runtime closure is eligible for `releases.lock.toml` only when its exact
candidate completes the phone gate. Packaging a recorded closure uses:

```sh
./fplinux package nokia-ta1618
```

The command rejects a runtime closure that is not recorded as qualified. A
qualified archive is written under `.cache/out/releases/` with `release` instead
of `candidate` in its name.

The final 16 hexadecimal characters in either filename identify the complete
package contents, not only `ramboot.bin`.

See [Building FPLinux](BUILDING.md) for source-build requirements and the
[target document](../targets/nokia-ta1618/README.md) for the phone-specific
qualification state.

## Archive contents

Every package contains one top-level directory:

```text
FPLinux-Nokia-3210-4G-TA1618-console-<status>-linux-x86_64-<content16>/
├── image/
│   └── ramboot.bin
├── assets/
│   ├── pinmap.bin
│   ├── keymap.bin
│   └── t117_fdl1.bin
├── host/
│   ├── spd_dump
│   ├── libc_server
│   └── fplinux-usb-console
├── licenses/
│   └── musl/
│       └── COPYRIGHT
├── runner/
│   ├── run.py
│   └── platform_adapter.py
├── runtime-manifest.json
├── assets.lock.toml
├── BUILD-MANIFEST.json
├── README.txt
├── SHA256SUMS
├── LICENSE
└── THIRD_PARTY_NOTICES.md
```

Candidate archives additionally contain the generated
`CANDIDATE-NOTICE.txt`. It is package metadata, not part of the runtime closure;
release archives omit it.

The target's `fplinux.release/v2` manifest is data-only. `bundle_files` names all
generated inputs that packaging verifies and includes. `runtime_files` selects the
subset whose bytes and executable modes form the hardware-qualified runtime
digest. `documents` names target release documents. `BUILD-MANIFEST.json`, asset
provenance, target documentation and fixed legal notices are archive inputs
outside the qualified runtime subset. Hardware qualification is the digest of
`runtime_files` only. Executable roles are not target-defined: packaging derives
the shared runner and typed host tools from the selected platform and requires
every executable, runtime asset, adapter and runtime manifest in `runtime_files`.
Project-level license documents are added by the fixed packager.

Only the files selected by the fixed packager and the target's data-only
allowlist enter the archive. Build trees, kernel debug files and caches are not
package inputs. `runtime-manifest.json` is the deterministic contract consumed
by `common/run.py`; the complete `target.toml` and `platform.toml` are not
packaged. The generic `fplinux.assets/v1` lock records asset sources, roles and
expected output hashes. ZIP timestamps, permissions and entry order are
normalized; an existing package name is never overwritten with different bytes.

## Run a Linux release archive

A Linux x86-64 release archive requires:

- Python 3.11 or newer;
- GNU `stdbuf` from coreutils;
- USB permissions for `1782:4d00` and `0525:a4a6`.

The bundled native host tools are static Linux/x86-64 executables; host glibc,
libusb and libudev packages are not runtime dependencies.

Extract the complete top-level directory, enter it and verify its contents:

```sh
cd FPLinux-Nokia-3210-4G-TA1618-console-release-linux-x86_64-<content16>
sha256sum -c SHA256SUMS
```

Power the phone off and disconnect USB, then start the runner:

```sh
./runner/run.py
```

Before asking for the phone, the runner checks its Python version and verifies
the hashes of the bundled static host tools. The UMS9117 adapter also checks for
`stdbuf` before printing the connection instructions.
When prompted, hold `*` and connect USB while keeping `*` pressed. After BootROM
USB appears, the adapter verifies read/write access to its actual device node
before executing the RAM loader. All loader transfers target volatile RAM.

`sha256sum -c` verifies the extracted archive, not the running phone. A source
checkout additionally provides `./fplinux verify <target>`, which first checks
its local bundle inputs, then compares the running kernel's content-derived
FPLinux suffix with `build-manifest.json`. This identifies the prepared Linux,
Alpine rootfs receipt, kernel configuration, DTB and bootstrap recipe. It does
not replace the phone-side hardware qualification gate.

## USB access

Both loader stages use libusb device nodes rather than `/dev/ttyUSB*`:

- `1782:4d00` is the Unisoc BootROM;
- `0525:a4a6` is the running Linux gadget: interface 0 is the shell and transfer
  channel, and interface 1 carries host keyboard events.

On a desktop system managed by logind, create a local udev rules file such as
`/etc/udev/rules.d/70-fplinux.rules`:

```udev
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="1782", ATTR{idProduct}=="4d00", TAG+="uaccess"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0525", ATTR{idProduct}=="a4a6", TAG+="uaccess"
```

For a headless host, use a distribution-appropriate local group and
`MODE="0660"` instead of `TAG+="uaccess"`. Do not make the devices world-writable
with `MODE="0666"`. Reload the rules, then disconnect and reconnect the phone:

```sh
sudo udevadm control --reload-rules
```

Run the FPLinux tools as the regular user. Do not work around missing udev
permissions by running the complete loader with `sudo`.

## Console lifecycle

The host USB client uses these controls:

- `Ctrl-]` detaches the host client only. It does not exit the phone shell,
  reboot Linux or power the phone off.
- `Ctrl-C` is forwarded to the shell on the phone.
- `exit` ends the active phone shell; the OpenRC-supervised USB getty starts a
  replacement shell.

Interface 1 can forward a host keyboard while interface 0 stays attached:

```sh
sudo ./host/fplinux-usb-console \
  --interface 1 --keyboard /dev/input/eventN
```

The forwarder uses `EVIOCGRAB`, so that keyboard stops reaching the host desktop
until the process exits.

Detach with `Ctrl-]` before unplugging USB. If Linux is running, reconnect
from the extracted archive with:

```sh
./host/fplinux-usb-console --interface 0
```

Do not start `runner/run.py` merely to reconnect: the full runner waits for a
BootROM device and starts a RAM-load sequence.

The Nokia TA-1618 power-off handler is qualified for battery-only shutdown.
Detach with `Ctrl-]`, disconnect USB and hold the red handset key continuously
for five seconds. Releasing it before the deadline cancels the request, and a
short press is an ordinary `KEY_POWER` input event.

Before starting system shutdown, the button handler verifies the exact SC2720
identity, checks that charger input is inactive and syncs the filesystems. The
final sys-off handler repeats the identity and charger guards, then requests
the PMIC hardware power-down sequence without reading the PMIC after its final
write. A connected charger refuses the long-press request while Linux is
running.

A failed final guard stops the shutdown path instead of attempting another PMIC
operation. If the phone is powered after shutdown starts, remove and reinsert
the battery before booting again. A successful shutdown discards the volatile
FPLinux session. Manual power-on uses the vendor boot path. Linux reboot is
unqualified.

## Troubleshooting

### The runner times out waiting for `1782:4d00`

Power the phone off, disconnect USB and start the runner first. Hold `*`, connect
USB only when prompted and keep `*` pressed until the BootROM device is found.
If `0525:a4a6` is already present, Linux is running; use
`./host/fplinux-usb-console --interface 0` instead.

### A USB device is visible but access is denied

Apply the rules in [USB access](#usb-access) for the ID named in the error and
reconnect the phone. Permissions are needed separately for both the BootROM and
Linux-console IDs.

### `stdbuf` is missing

Install GNU coreutils for the host distribution. The runner uses GNU `stdbuf` in
the fixed bootstrap handoff.

### A loader stage exits or the Linux console does not appear

For an archive, run `sha256sum -c SHA256SUMS` from the extracted top-level
directory. Keep the complete runner output: a non-zero FDL/bootstrap stage or a
Linux-console timeout means the candidate did not complete the documented path.
Do not treat it as qualified and do not package it as a release.

### The attached console disconnects

If the phone enumerates as `0525:a4a6`, run
`./host/fplinux-usb-console --interface 0` again. Starting the complete runner
is appropriate only for a power-off BootROM load.

## Release rule

A release archive must use a runtime closure listed in `releases.lock.toml` and
completed on that exact phone variant. The closure digest covers the image, host
binaries, shared runner, fixed platform adapter, assets and runtime manifest. The
kernel suffix identifies the prepared Linux, Alpine rootfs receipt, kernel
configuration, DTB and bootstrap recipe. Packaging separately checks every
generated bundle file against the successful-build manifest. Distribute the
corresponding source with the binary ZIP.

The archive must also include the notices declared by the fixed packager,
including the complete musl copyright and permission notice.
