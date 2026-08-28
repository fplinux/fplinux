FPLinux microSD boot candidate for Nokia 3210 4G (TA-1618)

The microSD boot payload in this archive has completed hardware qualification
on Nokia 3210 4G (TA-1618). The archive remains a candidate, not a release.
Keep CANDIDATE-NOTICE.txt with any test copy and do not publish it as a release.

The microSD image is FPLINUX.img.xz. Writing it replaces the card's
partition table and files. Open that file with Raspberry Pi Imager, choose
Use Custom, select the intended microSD card and let the imager verify the
write. The image contains:

  - p1: 64 MiB FAT32, label FPLBOOT, with FPLINUX.ITB;
  - p2: an initial 64 MiB ext4 volume named FPLROOT, used as the writable
    Linux root.

During boot, Linux extends p2 to the end of the card and grows ext4 online.
Repeating the boot when both already fill the card is a no-op. Do not resize,
move or edit either partition manually.

After writing the image, insert the card and leave it installed. Read
docs/guides/STANDALONE.md for the host and USB setup. Power the phone off,
disconnect USB and start:

  ./runner/run.py

When the runner asks for the phone, hold * (asterisk) and connect USB. The
remaining microSD boot is automatic and does not require keyboard input. Keep
USB connected until Linux starts and the SSH session becomes ready.

The RAM loader, resident stage0 and full U-Boot do not alter NAND, NV data or
the stock boot chain. Starting the phone without a fresh USB RAM load returns
to the stock boot path.

The card is the running system disk. Never remove it while Linux is running,
even after sync. Do not mount FPLBOOT at /mnt/card or use the ordinary Nokia
FAT32 hot-swap instructions. This archive also bundles the ordinary optional
application APKs, but their /mnt/card workflows are not qualified for this
profile.

To end the session, exit applications and SSH, disconnect USB and any charger,
then hold the red handset key continuously for five seconds. Wait until the
phone is fully off; the orderly shutdown remounts the system card read-only
before power is removed. See docs/target/POWER_OFF.md and
docs/target/MICROSD.md for the profile-specific rules.
