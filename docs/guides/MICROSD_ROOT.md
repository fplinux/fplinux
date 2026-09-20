# microSD system root

The `microsd-uboot` profile uses a removable microSD card as the writable Linux
system disk. It has the same application and peripheral interfaces as the
default RAM profile. Check the selected target's support status before using
this boot mode on hardware.

## Prepare the card

Building this profile produces `FPLINUX.img.xz`; the build does not write a
card. The file contains a complete MBR disk image, not a file to copy onto an
existing filesystem. Writing it replaces the selected card's partition table
and files. Select the intended removable card explicitly and verify the write
by reading it back before booting.

The image contains:

- partition 1: 64 MiB FAT32, label `FPLBOOT`, containing `FPLINUX.ITB`;
- partition 2: an initial 64 MiB ext4 filesystem, label `FPLROOT`, used as `/`.

The root identifier is `PARTUUID=46504c58-02`. During boot, Linux extends
partition 2 to the end of the card and grows ext4 online. On subsequent boots,
an already full partition and filesystem require no changes.

Do not write this image from a system whose root is on the destination card.
To rewrite a card through the phone, first load the default RAM profile and
verify the root and card identity, stop applications using the card, and
unmount all its filesystems. The complete image must be read back and compared
before the next microSD boot.

## Boot

From a source checkout, follow the target's loading instructions, selecting
`--profile microsd-uboot` or `--boot microsd`. Insert the prepared card before
starting the phone and keep it installed.

For a standalone archive containing `FPLINUX.img.xz`, prepare the card first,
then follow [the archive instructions](STANDALONE.md). Its runner already
selects the image packaged with that archive.

The USB loader places U-Boot in RAM; U-Boot loads Linux from the card. This
does not replace the stock boot chain or install a bootloader in internal
storage. A fresh USB load is required for each cold FPLinux start.

## Persistence and shutdown

Installed applications, pairing records, and files on the ext4 root survive a
cold boot. Files in temporary RAM filesystems such as `/tmp` do not. Keep
`FPLBOOT` reserved for the boot FIT; do not mount it as application storage at
`/mnt/card`.

The system card cannot be unmounted or hot-swapped while Linux is running.
`sync` alone does not make it safe to remove the card or cut power. Use the
[power-key shutdown procedure](../features/POWER_OFF.md#safe-shutdown) only
on a phone whose documentation lists battery-only power-off as supported. Wait for shutdown to finish
before removing or rewriting the card.

### Shutdown with USB power

On INOI, battery-only power-off has not been tested. A `poweroff` request or a lost
USB connection alone does not establish that the card is safe to remove.
Keep USB connected while stopping applications and services that write to the
card. Disable all card-backed swap and unmount every card filesystem except
the system root, following [safe removal](../features/MICROSD.md#safe-removal).
Then run in the phone's root shell:

```sh
sync
mount -o remount,ro /
```

The remount must succeed, and `/proc/mounts` must show the ext4 `/` mount with
the `ro` option. If the remount fails, stop the remaining writers and try again;
leave the phone powered until all card filesystems are unmounted or read-only.
Only after that condition is established, request shutdown with `poweroff`
and disconnect power. This sequence protects the card; it does not demonstrate
battery-powered operation or the phone's physical power-off control.
