# Removable microSD storage

Use this interface in the default RAM profile on a phone with supported
microSD storage. The selected target's documentation states which filesystems
and card-removal behavior are supported; sharing this interface does not
establish support on another phone. In a standalone archive, consult its
`README.txt` for the target's support status.

In `microsd-uboot`, the card is the system root and must remain installed. Use
the [microSD system-root instructions](../guides/MICROSD_ROOT.md) instead of
the removable-storage operations below.

## Interface

The card appears through the standard Linux block interface. Check the
partition layout and filesystem before mounting; do not format or overwrite
the card to mount existing data. Use the first partition when present, or the
whole-card node for a filesystem without a partition table:

```sh
card=/dev/mmcblk0p1
[ -b "$card" ] || card=/dev/mmcblk0
mkdir -p /mnt/card
```

For FAT32 storage on a target that supports it:

```sh
mount -t vfat -o rw "$card" /mnt/card
```

For ext4 storage on a target that supports it:

```sh
mount -t ext4 -o rw "$card" /mnt/card
```

For FAT32 data that does not need writes, mount it read-only:

```sh
mount -t vfat -o ro,nodev,nosuid,noexec,utf8=1 "$card" /mnt/card
```

## Safe removal

Stop applications using the card and disable any swap backed by it with
`swapoff` before unmounting. Flush and unmount every card filesystem before
removing the card or ending the RAM session:

```sh
sync
umount /mnt/card
```

Only targets whose documentation lists unmounted hot-swap as supported allow
removal and reinsertion without restarting Linux. Never remove a mounted card
or a card with active swap. Removal during a write, filesystem repair, erase or
discard is not a supported workflow.

Card detection is polled. Wait for the block node to appear or disappear; no
exact detection delay is part of the supported interface.

This writable-storage support covers removable microSD only; it does not
provide a write or restore path for internal phone storage.
[TyrQuake](../apps/TYRQUAKE.md) and
[MicroPythonOS](../apps/MICROPYTHONOS.md) describe their application storage
interfaces and limits.
