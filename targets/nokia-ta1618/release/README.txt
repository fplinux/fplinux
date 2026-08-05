FPLinux for Nokia 3210 4G (TA-1618)

This archive contains a volatile-RAM FPLinux payload and its fixed host runner.
The runner has no flash, erase, partition or NV operation and does not modify
the vendor firmware or phone storage. The archive filename identifies whether
it is a hardware-qualification candidate or a qualified release. If
CANDIDATE-NOTICE.txt is present, the archive is not a release.

Host requirements:
  - Linux x86-64 with glibc 2.38 or newer
  - Python 3.11 or newer
  - libusb 1.0, libudev and GNU coreutils (stdbuf)
  - USB permissions for devices 1782:4d00 and 0525:a4a6

Start:
  1. Extract the complete top-level directory.
  2. Enter that extracted directory.
  3. Run: sha256sum -c SHA256SUMS
  4. Power the phone off and disconnect USB.
  5. Run: ./runner/run.py
  6. Hold * and connect USB while keeping * pressed when prompted.

The runner verifies the bundled board assets and host-tool dependencies before
asking for the phone. It then checks BootROM USB access before attempting the
fixed FDL1, RAM-payload and Linux USB-console sequence. Ctrl-] detaches the host
console; it does not reboot or power off the phone. To reconnect while Linux is
still running, use ./host/fplinux-usb-console instead of the full runner.

To end the RAM session, detach with Ctrl-], disconnect USB, remove the battery
and then reinsert it. The next normal power-on uses the unchanged vendor
firmware. Linux reboot, poweroff and PMIC-controlled shutdown are not qualified
exit paths.

BUILD-MANIFEST.json records the content-addressed workspace and toolchain
receipts together with bundled-file hashes. Exact pinned inputs are defined by
the corresponding source snapshot and its lock files.
