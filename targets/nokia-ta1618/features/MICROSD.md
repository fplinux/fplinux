# microSD on Nokia 3210 4G (TA-1618)

This page applies only to Nokia 3210 4G (TA-1618). FPLinux supports FAT32
read/write and unmounted card insertion, removal and reinsertion while Linux
remains running.

## Interface

The card appears through the standard Linux block interface. Use the first
partition when present, or the whole-card node otherwise:

```sh
card=/dev/mmcblk0p1
[ -b "$card" ] || card=/dev/mmcblk0
mkdir -p /mnt/card
mount -t vfat -o rw "$card" /mnt/card
```

For data that does not need writes, mount it read-only:

```sh
mount -t vfat -o ro,nodev,nosuid,noexec,utf8=1 "$card" /mnt/card
```

## Safe removal

Flush and unmount the filesystem before removing the card or ending the RAM
session:

```sh
sync
umount /mnt/card
```

After `umount`, the card may be removed and reinserted without restarting
Linux. Never remove a mounted card. Removal during a write, filesystem repair,
erase or discard is not a supported workflow.

Card detection is polled. Wait for the block node to appear or disappear; no
exact detection delay is part of the supported interface.

This support covers removable microSD only. The default Nokia target and release
workflows do not expose internal storage. The development-only `nand-ro-lab`
profile can capture one read-only physical backup; it provides no filesystem
mount, write or restore path. The TyrQuake and MicroPythonOS pages under
`docs/apps/` describe how those applications use the card.
