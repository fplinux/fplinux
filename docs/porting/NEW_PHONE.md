# Bring up a new UMS9117 phone

This route takes a Unisoc UMS9117 (T117) phone that has no FPLinux target to a
Linux session in RAM, a complete read-only backup of its internal NAND and a
board report extracted from the stock firmware in that backup. The new target
has no display, keypad, audio or Bluetooth support: the phone shows no boot
screen or backlight, and you use it through the USB session from the host. The
route ends with what to report about the phone.

Run every command from the root of the source checkout.

## Before you start

- Prepare a Linux x86-64 host as described in
  [Requirements and setup](../guides/BUILDING.md#requirements-and-setup).
- Install the udev rules from [USB access](../guides/LOADING.md#usb-access).
- Have a USB data cable for the phone.

The route needs no files from the stock firmware: the loader uses the FDL1
that the project supplies, and FPLinux reads the NAND itself. The loader writes
only volatile RAM, and the NAND reader cannot write or erase the NAND.

## Create a headless target

Choose a target name and the phone's public brand and product names as
described in [Create a new target](../guides/BUILDING.md#create-a-new-target).
The device name, which is the brand and product separated by one space, must
not exceed 31 characters; the command refuses a longer name.

```sh
./fplinux target new <target> --brand <brand> --product "<product>"
```

The command creates `targets/<target>` and prints:

```text
Created headless target targets/<target>:
  README.md
  bootstrap/Makefile
  bootstrap/main.c
  bootstrap/payload.s
  linux/arch/arm/mach-ums9117/Kconfig
  linux/config.fragment
  linux/dts/ums9117-<target>.dts
  linux/dts/unisoc.Makefile
  loader/assets.lock.toml
  release/README.txt
  release/manifest.toml
  target.toml
Next: ./fplinux build <target> && ./fplinux run <target>
```

The new target names `*` as the boot key and sets `exec_distance = 0` in the
`[adapter]` table of `targets/<target>/target.toml`.

## Boot without a screen

Build the target:

```sh
./fplinux build <target>
```

Power the phone off and disconnect USB. Start the loader before connecting the
phone:

```sh
./fplinux run <target>
```

Wait until the loader asks for the phone. Only then hold `*` and connect the
powered-off phone, keeping `*` pressed as the loader instructs. The general
loader rules are in [Connect the phone](../guides/LOADING.md#connect-the-phone).

A successful boot prints these lines, with diagnostic lines between them:

```text
<Brand> <Product> console RAM boot
Operations: RAM FDL1 load, RAM payload load.
There are no flash, erase, partition, or NV commands.
Headless target: the phone shows no boot screen or backlight; progress is reported here.

Power the phone off, hold *, then connect USB while keeping * pressed.
Waiting up to 300 seconds for BootROM USB 1782:4d00...
BSL_REP_VER: "SPRD3\0"
BSL_REP_VER: "Custom FDL1: CHIP ID = 0x98180001\0"
<TARGET>_LINUX_BOOTSTRAP event=stage stage=0 ram=0x04000000 zimage=... dtb=... message=ENTRY
<TARGET>_LINUX_BOOTSTRAP event=stage stage=1 message=NO DISPLAY
<TARGET>_LINUX_BOOTSTRAP event=stage stage=2 message=SPRD TIMER OK
<TARGET>_LINUX_BOOTSTRAP event=stage stage=3 message=COPY KERNEL
<TARGET>_LINUX_BOOTSTRAP event=stage stage=4 message=COPY DTB
<TARGET>_LINUX_BOOTSTRAP event=stage stage=5 message=PREPARE LINUX
Bridge acknowledged the Linux transition; waiting up to 60 seconds for Linux USB-NCM 0525:a4a6.
SSH host key observed; validating the private RAM session identity.
Private USB-NCM SSH session is ready.
```

The first `BSL_REP_VER` line comes from the phone's BootROM. The second comes
from FDL1 once it runs; its `CHIP ID` identifies the SoC, and the supported
phones report `0x98180001`. The `<TARGET>_LINUX_BOOTSTRAP` lines are the
bootstrap stages, prefixed with the target name in capitals. `ram=0x04000000`
is the 64 MB of RAM that FPLinux requires, and `NO DISPLAY` confirms that the
target starts without a screen.

From an interactive terminal, `run` then opens a shell on the phone. Leaving the
shell does not end Linux; see [After boot](../guides/LOADING.md#after-boot).
Keep the complete `run` output for the report.

### The loader does not see the phone

If no `BSL_REP_VER` line appears after you connect the phone, the phone did not
start in BootROM mode. The loader stops after 300 seconds with:

```text
fplinux run: BootROM USB was not detected
```

When the phone appears on USB only briefly, the loader stops with:

```text
fplinux run: BootROM USB 1782:4d00 repeatedly disconnected before it could be opened
```

Disconnect the phone, power it off and start again with `run`, holding the key
before and while you connect USB. If `*` never works, the phone uses another
BootROM key: try another key in the same sequence. When a key works, replace
`*` with it in `boot_instructions` in the `[adapter]` table of
`targets/<target>/target.toml` and build again. If the loader reports that the USB device is not readable and
writable, follow
[USB is visible but access is denied](../guides/LOADING.md#usb-is-visible-but-access-is-denied).

### FDL1 does not start

When the BootROM `BSL_REP_VER` line appears but the `Custom FDL1` line never
does, the loader attempt fails. `run` asks you to disconnect, power off and
reconnect the phone after each failed attempt, and stops after the third one
with `fplinux run: RAM loader failed after 3 attempts`.

A new target starts with `exec_distance = 0`. If FDL1 does not start with it,
set this in the `[adapter]` table of `targets/<target>/target.toml`:

```toml
exec_distance = 0x314d
```

Then build and run the target again. The loader lists the extra operation:

```text
Operations: exec-distance setup, RAM FDL1 load, RAM payload load.
```

Like every loader operation, the setup writes only volatile RAM, so it is safe
to try. No value other than `0` and `0x314d` is known to start FDL1.

### Loading stops after FDL1 starts

FDL1 prepares the phone's RAM when the loader sends the FPLinux payload. It
sets up the 64 MB of LPDDR2 memory used by the supported phones, and FPLinux
requires 64 MB of RAM. When a phone has another SoC variant or memory, the
loader attempts fail after the `Custom FDL1` line, or the bootstrap stops with
this record:

```text
<TARGET>_LINUX_BOOTSTRAP event=error error=2 message=64MB RAM REQUIRED
```

FPLinux does not start on such a phone. Report it with the complete `run`
output and photos of the markings on its SoC and flash chip.

### Linux USB does not appear

After `PREPARE LINUX`, the phone leaves BootROM USB `1782:4d00` and Linux
starts its USB device `0525:a4a6`. When the boot stops before
`Private USB-NCM SSH session is ready.`, `run` ends with an error such as:

```text
fplinux run: bridge did not acknowledge the Linux transition before the deadline
fplinux run: BootROM USB did not disconnect before the deadline
fplinux ssh: the exact USB-NCM SSH session did not become ready before the deadline
```

Check whether Linux USB is present:

```sh
lsusb -d 0525:a4a6
```

If the device is listed, follow
[Linux USB appears but the console does not connect](../guides/LOADING.md#linux-usb-appears-but-the-console-does-not-connect).
If it is not, report the phone with the complete `run` output.

## Identify the NAND chip

While the phone session runs, use a second host terminal, or leave the phone
shell, and run:

```sh
./fplinux nand identify <target>
```

The command prints the report of the phone's NAND reader, one `key=value` line
each. For a phone with a DS35M1GA chip it reads:

```text
id_bytes=e521
chip=DS35M1GA
page_main_bytes=2048
oob_bytes=64
pages_per_block=64
block_count=1024
raw_bytes=138412032
geometry_source=table
feature_a0=0x38
feature_b0=0x10
feature_c0=0x00
```

`id_bytes` holds the chip's ID bytes, manufacturer byte first.
`geometry_source=table` means that the reader identified the chip from its
list of supported chips.
[Save a NAND backup](../guides/BUILDING.md#save-a-nand-backup) describes every
field.

FPLinux has read FM25LG01B and DS35M1GA chips on supported phones. The reader
also identifies other chips that no supported phone uses.

### The chip is not identified

For a chip that the reader does not know, the report shows `chip=unknown`,
zero sizes and `geometry_source=unknown`. It still shows the chip's `id_bytes`
and its `feature_a0`, `feature_b0` and `feature_c0` values. A backup is not
possible: `nand backup` stops before reading any page with:

```text
fplinux: the running kernel does not identify this NAND chip: id_bytes=... feature_a0=... feature_b0=... feature_c0=...
```

Report the phone with the complete `nand identify` output and a photo of the
markings on the flash chip.

### The chip is new to FPLinux

When `chip` names a chip other than FM25LG01B or DS35M1GA, the reader
identifies it and `nand backup` proceeds, but no FPLinux phone has used that
chip before: its entry in the reader's list has not been checked against a
real chip. Save the backup and state in the report that the chip is new.

## Save a NAND backup

```sh
./fplinux nand backup <target> nand.bin
```

The command reads the complete NAND through the running session, which takes
several minutes, and reports the two files it writes:

```text
NAND backup saved: nand.bin (138412032 bytes, sha256=...)
NAND geometry saved: nand.bin.json (chip DS35M1GA, id_bytes e521)
```

`nand.bin` holds every page's main and OOB bytes. `nand.bin.json` is the
geometry receipt: the chip identity, the page layout, and the size and digest of
the backup. Both files have mode `0600`. The backup contains the stock firmware
and the phone's own data; keep it private.
[Save a NAND backup](../guides/BUILDING.md#save-a-nand-backup) describes the
receipt fields.

When you are done, end the session as described in the
`End the RAM session` section of `targets/<target>/README.md`.

### The backup stops early

When the transfer stops, for example because USB was disconnected or you
pressed Ctrl+C, the command fails and publishes nothing. A backup saved earlier
at the same path and its receipt stay unchanged. The error names the failed
SSH command or the short image, for example:

```text
fplinux: incomplete raw NAND image: expected ... bytes, got ...
```

Run `nand backup` again. If the phone no longer runs the session, start again
from a powered-off phone with `run`.

## Prepare the board report

The new target declares the board maps, which the platform extracts from the
stock firmware in the backup. Prepare them from the saved backup; the phone does
not need to be connected, but the target must have a current build:

```sh
./fplinux device-data prepare <target> --from-dump nand.bin
```

The command reads `nand.bin` with its receipt `nand.bin.json` and prints the
path of the board report:

```text
Review <checkout>/.cache/device-data/<target>/generations/<generation>/reports/board-maps/board-report.json.
```

[UMS9117 board data](../../platforms/ums9117/README.md#board-data-from-the-stock-firmware)
describes what the report contains and which decisions it leaves to a person.

## Report a new phone

Open a
[new phone report](https://github.com/fplinux/fplinux/issues/new?template=new-phone.yml)
in the project issue tracker. The form asks for the phone's name and boot key,
its `lsusb` line taken in a second terminal while the phone is connected with
the boot key held, the complete `run` and `nand identify` output, the geometry
receipt, the board report and photos of the markings on the SoC and the flash
chip.

Do not attach the NAND backup or any stock firmware image publicly. They
contain the phone's own data and the vendor's firmware.

## Complete the target

The target is completed by hand from the backup and the board report; see the
[phone target template](TARGET.md).
