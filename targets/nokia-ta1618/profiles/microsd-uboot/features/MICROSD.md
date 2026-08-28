# microSD system card on Nokia 3210 4G (TA-1618)

This page applies to the public `microsd` boot mode. The card is the Linux
system disk, not removable application storage.

## Card layout

`FPLINUX.img.xz` contains a complete MBR image:

- partition 1 is a 64 MiB FAT32 volume named `FPLBOOT` containing
  `FPLINUX.ITB`;
- partition 2 starts as a 64 MiB ext4 volume named `FPLROOT`, mounted
  read/write as `/`.

The fixed root identifier is `PARTUUID=46504c58-02`. On boot, the system grows
partition 2 to the end of the card and then grows ext4 online. The same service
runs safely on later boots: an already full partition and filesystem need no
marker or migration state. Writing the image replaces the existing partition
table and files on the selected card.

## Runtime rules

Keep the card installed for the whole Linux session. It cannot be unmounted or
hot-swapped because partition 2 is the active root filesystem. `sync` alone
does not make removal safe.

Partition 1 is reserved for the boot FIT. Do not mount it at `/mnt/card`, add
application data to it or alter `FPLINUX.ITB` while qualifying this candidate.
The ordinary Nokia FAT32 application-storage and hot-swap procedures do not
apply to this profile.

Shut the phone down as described in [Power-off](POWER_OFF.md). The shutdown path
remounts the system card read-only before power is removed. Wait until the phone
is fully off, and only then remove or rewrite the card. Internal phone storage
is not used or modified by this boot path.
