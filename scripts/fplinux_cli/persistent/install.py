# SPDX-License-Identifier: GPL-2.0-only
"""Host-driven persistent boot: discover, back up, install, verify, restore.

The RAM boot FPLinux ships today never touches flash: the launcher
downloads the pinned FDL1 and the FPLinux stage over USB and runs
them from SDRAM. This module turns the same protocol stack into an
installer whose on-hardware flash stage exists as the C extension
the design note sketches (the read-only UMS9117 NAND driver extended
with the write-gated operations ``ENABLE_WRITE_FLASH`` enables).

Flow, exactly the commands the running stages accept:

1. BootROM handshake (vendor USB start request, one ``0x7e`` sync
   byte, ``SPRD3`` version, CONNECT, CRC-16 frames).
2. FDL1 download to 0x6200 and EXEC (plain, or the T117 exec-distance
   workaround frame when the target configures one).
3. Flash-stage download to 0x80100000 and EXEC, acknowledged by
   nothing: the stage only speaks again when the host syncs.
4. ``discover``: sync with the flash stage, switch to the word-sum
   checksum, read both boot regions using the same extent rule
   spd_dump's ``dump_flash`` auto mode uses (DHTB header + payload,
   then the optional 0x60-byte signature header and signature blob).
5. The safety gates: a region whose header is not a stock-like DHTB
   image, a stock chain that carries signature blocks, or a boot0
   that already holds the FPLinux nandboot stage makes the installer
   refuse before anything is written.
6. ``install``: program the FPLinux nandboot stage, DHTB-wrapped, to
   boot0, then the U-Boot DHTB image to boot1, never both in one
   transaction, so a power cut leaves half of the previous chain
   alive and the phone bootable by the boot key.
7. ``verify`` reads both regions back and compares digests.
8. ``restore`` programs the saved stock bytes of each region back,
   with the same transaction shape.

Nothing here writes the user data: the stock kernel and user regions
are never erased, so a phone booted with the boot key held always
comes back to the need-USB flow, and the restore is byte-exact.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections.abc import Callable
from dataclasses import dataclass, field

from . import dhtb
from .client import DownloadClient, ProtocolError
from .nandsim import (
    REGION_BOOT0,
    REGION_BOOT1,
    REGION_KERNEL,
    SimulatedDevice,
    UMS9117_CHIP_ID,
)

FDL1_ADDRESS = 0x6200
STAGE_ADDRESS = 0x80100000
PROBE_BYTES = 0x34
DHTB_SIGNATURE_HEADER_BYTES = 0x60
ITB_MAGIC = 0xD00DFEED


class InstallError(Exception):
    """The installer refused to continue; the phone is untouched."""


@dataclass(frozen=True)
class StageImages:
    """The FPLinux images one install writes, never the stock ones.

    ``nandboot`` is the stage the BootROM loads from boot0 on every
    power-on: it serves the download protocol when the boot key is
    held (the same role the pinned ``t117_fdl1.bin`` plays in the RAM
    flow) and chains to boot1 otherwise. ``uboot`` is the U-Boot
    payload of the microsd-uboot profile. ``flash_stage`` is the
    host-download stage carrying the write-gated NAND operations.
    """

    nandboot: bytes
    uboot: bytes
    flash_stage: bytes


@dataclass
class RegionBackup:
    """One saved boot region: raw bytes, extent, digests."""

    region: int
    image: bytes
    contains_signature: bool = False
    payload_sha256: str = ""

    def __post_init__(self) -> None:
        try:
            parsed = dhtb.Image(self.image)
        except ValueError:
            return
        self.payload_sha256 = hashlib.sha256(parsed.payload).hexdigest()


@dataclass
class BootBackup:
    """The mandatory pre-install backup of both boot regions."""

    boot0: RegionBackup
    boot1: RegionBackup
    chip_id: int

    def sha256_hex(self, region: int) -> str:
        region_backup = self.boot0 if region == REGION_BOOT0 else self.boot1
        return hashlib.sha256(region_backup.image).hexdigest()

    def dump_receipt(self, path: str) -> None:
        """Write the backup receipt, the restore's proof of custody."""
        receipt = {
            "chip_id": f"0x{self.chip_id:08x}",
            "boot0": {
                "image_sha256": self.sha256_hex(REGION_BOOT0),
                "payload_sha256": self.boot0.payload_sha256,
                "size": len(self.boot0.image),
                "contains_signature": self.boot0.contains_signature,
            },
            "boot1": {
                "image_sha256": self.sha256_hex(REGION_BOOT1),
                "payload_sha256": self.boot1.payload_sha256,
                "size": len(self.boot1.image),
                "contains_signature": self.boot1.contains_signature,
            },
        }
        with open(path, "w", encoding="utf-8") as receipt_file:
            json.dump(receipt, receipt_file, indent=2, sort_keys=True)
            receipt_file.write("\n")


# Stage actions: what each image does when it runs.


def make_nandboot_action() -> Callable[[SimulatedDevice], str]:
    """Dual-role boot0 stage: download mode, or the chain to boot1.

    With the boot key held, the BootROM hands control to the nandboot
    stage in USB download mode, exactly like the RAM flow's FDL1:
    word-sum frames, START/MIDST/END downloads, EXEC into the flash
    stage. Without the key, the stage does what the stock FDL1 does at
    power-on: read boot1, check its DHTB digest, hand control to
    U-Boot.
    """

    def action(device: SimulatedDevice) -> str:
        if device.usb_session:
            device.switch_stage("fdl1")
            device.log("nandboot-download-mode")
            return "fdl1"
        first, count = device.regions.region_of(REGION_BOOT1)
        region_bytes = device.regions.read(REGION_BOOT1, 0, count * device.nand.block_bytes)
        try:
            image = dhtb.Image(region_bytes)
        except ValueError:
            device.log("boot1-invalid")
            return "boot1-invalid"
        if not image.verified:
            device.log("boot1-sha-mismatch")
            return "boot1-sha-mismatch"
        boot_action = device.exec_handlers.get(hashlib.sha256(image.payload).hexdigest())
        if boot_action is None:
            device.log("boot1-unhandled")
            return "boot1-unhandled"
        device.log(f"boot1-ok size={image.size:#x}")
        return boot_action(device)

    return action


def make_uboot_action() -> Callable[[SimulatedDevice], str]:
    """The U-Boot stage of the microsd-uboot profile: read FPLINUX.ITB."""

    def action(device: SimulatedDevice) -> str:
        device.log("uboot-start")
        card = device.sd_card or {}
        blob = card.get("FPLBOOT/FPLINUX.ITB")
        if blob is None:
            device.log("uboot-no-itb")
            return "uboot-shell"
        magic = int.from_bytes(blob[:4], "big")
        if magic != ITB_MAGIC:
            device.log(f"uboot-bad-itb magic={magic:#x}")
            return "uboot-bad-itb"
        device.log(f"linux-session itb={len(blob)}")
        return "linux-session"

    return action


def make_flash_stage_action() -> Callable[[SimulatedDevice], str]:
    """The flash stage: HOST download commands now touch boot regions."""

    def action(device: SimulatedDevice) -> str:
        device.switch_stage("flash")
        device.log("flash-stage-entered")
        return "flash"

    return action


def make_stock_action() -> Callable[[SimulatedDevice], str]:
    """A stock stage, known to the simulator by its payload digest."""

    def action(device: SimulatedDevice) -> str:
        device.log("stock-kernel-boot")
        return "stock-rtos"

    return action


def bind_stage_actions(
    device: SimulatedDevice,
    images: StageImages,
    *,
    stock_boot0: bytes | None = None,
    stock_boot1: bytes | None = None,
    stock_kernel: bytes | None = None,
) -> None:
    """Register every stage image the simulated phone can run.

    The one registry serves both paths, keyed by payload digest: the
    download chain (nandboot in FDL1 role, flash stage) and the
    power-on chain (nandboot, U-Boot, stock stages).
    """
    device.register_stage_image(dhtb.wrap(images.nandboot), make_nandboot_action())
    device.register_stage_image(dhtb.wrap(images.uboot), make_uboot_action())
    device.register_stage_image(dhtb.wrap(images.flash_stage), make_flash_stage_action())
    for stock_payload in (stock_boot0, stock_boot1, stock_kernel):
        if stock_payload is not None:
            device.register_stage_image(dhtb.wrap(stock_payload), make_stock_action())


# Region reading: the discovery extent rule, ported from dump_flash.


def read_region_image(client: DownloadClient, region: int) -> RegionBackup:
    """Read one boot region with spd_dump's auto extent rule.

    The probe must find the DHTB magic and the little-endian version,
    exactly like ``dump_flash``; the image is header plus payload,
    plus the optional signature header and blob whose length fields
    must all agree. Anything else is a layout this installer does not
    recognize, and discovery refuses before writing anything.
    """
    probe = client.read_flash(region, 0, PROBE_BYTES)
    if probe[:4] != dhtb.MAGIC or int.from_bytes(probe[4:8], "little") != dhtb.VERSION:
        raise InstallError(f"region {region:#010x} has no DHTB header: refusing")
    size = int.from_bytes(probe[dhtb.SIZE_OFFSET : dhtb.SIZE_OFFSET + 4], "little")
    if size & 0x80000000:
        raise InstallError(f"region {region:#010x} DHTB size has the top bit set: refusing")
    extent = dhtb.HEADER_BYTES + size
    image = bytearray(client.read_flash(region, 0, extent))
    # Optional stock signature: a 0x60-byte header whose data-size,
    # data-offset, sign-size and sign-offset fields must agree with
    # the image we just measured, followed by the blob itself.
    signature_header = client.read_flash(region, extent, DHTB_SIGNATURE_HEADER_BYTES)
    data_size = int.from_bytes(signature_header[0x10:0x14], "little")
    data_offset = int.from_bytes(signature_header[0x18:0x1C], "little")
    sign_size = int.from_bytes(signature_header[0x20:0x24], "little")
    sign_offset = int.from_bytes(signature_header[0x28:0x2C], "little")
    if (
        data_size in (0, 0xFFFFFFFF)
        or data_offset != dhtb.HEADER_BYTES
        or data_size != size
        or sign_size >> 12
        or sign_offset != extent + DHTB_SIGNATURE_HEADER_BYTES
    ):
        return RegionBackup(region, bytes(image))
    signature = client.read_flash(region, extent + DHTB_SIGNATURE_HEADER_BYTES, sign_size)
    return RegionBackup(region, bytes(image + signature_header + signature), contains_signature=True)


@dataclass
class InstallationReport:
    """Digests and sizes the install recorded, for the receipt file."""

    steps: list[str] = field(default_factory=list)
    boot0_sha256: str = ""
    boot1_sha256: str = ""
    verify_sha256: dict[str, str] = field(default_factory=dict)


def _region_label(region: int) -> str:
    return "boot0" if region == REGION_BOOT0 else "boot1"


class Installer:
    """One installer session against any byte transport.

    The transport is the same callable the host client takes: the
    simulated device's ``transfer``, or the libusb endpoint adapter on
    hardware. All program paths run through the flash stage's write
    gate, so nothing flashes unless the full BROM-to-flash-stage chain
    completed first. The session stays up across discover, install,
    verify and restore, mirroring spd_dump's single-process flow; a
    device power cycle ends it, and the next operation opens a new
    one.
    """

    def __init__(
        self,
        transport: Callable[[bytes], bytes],
        images: StageImages,
        *,
        exec_distance: int = 0,
    ) -> None:
        self.transport = transport
        self.images = images
        self.exec_distance = exec_distance
        self.client = DownloadClient(transport)
        self.client.verbose = None
        self._flash_stage_ready = False

    # -- session plumbing -------------------------------------------------

    def _ensure_flash_stage(self) -> DownloadClient:
        """BROM, FDL1, flash stage: the RAM flow, then the write gate."""
        if self._flash_stage_ready:
            return self.client
        client = self.client
        client.vendor_start()
        client.brom_handshake()
        client.download(dhtb.wrap(self.images.nandboot), FDL1_ADDRESS)
        client.exec_download(FDL1_ADDRESS, distance=self.exec_distance)
        client.stage_handshake()
        client.download(dhtb.wrap(self.images.flash_stage), STAGE_ADDRESS)
        client.exec_download(STAGE_ADDRESS, expect_ack=False)
        client.stage_handshake()
        self._flash_stage_ready = True
        return client

    def close(self) -> None:
        """Mark the session ended: the device was power-cycled."""
        self._flash_stage_ready = False
        self.client = DownloadClient(self.transport)
        self.client.verbose = None

    def _program(self, region: int, image: bytes) -> None:
        try:
            self._ensure_flash_stage().program_region(region, image)
        except ProtocolError as error:
            raise InstallError(f"program {region:#010x} failed: {error}") from None

    def _read(self, region: int, size: int) -> bytes:
        try:
            return self._ensure_flash_stage().read_flash(region, 0, size)
        except ProtocolError as error:
            raise InstallError(f"read {region:#010x} failed: {error}") from None

    # -- the feature ------------------------------------------------------

    def discover(self) -> BootBackup:
        """Read, validate and back up the installed chain.

        Refused states, all checked before any write: a region
        without a stock-like DHTB header, a signed stock chain
        (signature blocks mean the boot ROM may enforce them, which
        unsigned FPLinux stages would trip), and a boot0 that already
        carries the FPLinux nandboot stage.
        """
        client = self._ensure_flash_stage()
        backup = BootBackup(
            boot0=read_region_image(client, REGION_BOOT0),
            boot1=read_region_image(client, REGION_BOOT1),
            chip_id=client.chip_id,
        )
        if backup.chip_id != UMS9117_CHIP_ID:
            raise InstallError(f"unexpected chip id {backup.chip_id:#010x}")
        for region_backup in (backup.boot0, backup.boot1):
            if region_backup.contains_signature:
                raise InstallError(
                    f"{_region_label(region_backup.region)} carries signature blocks; "
                    "flashing unsigned stages could brick the phone: refusing"
                )
            if not region_backup.payload_sha256:
                raise InstallError(
                    f"{_region_label(region_backup.region)} has an unreadable DHTB image: refusing"
                )
        if backup.boot0.payload_sha256 == hashlib.sha256(self.images.nandboot).hexdigest():
            raise InstallError(
                "boot0 already contains the FPLinux nandboot stage; restore the stock "
                "backup before installing again"
            )
        return backup

    def install(self, backup: BootBackup) -> InstallationReport:
        """Write nandboot to boot0 and U-Boot to boot1, two commits."""
        boot0_image = dhtb.wrap(self.images.nandboot)
        boot1_image = dhtb.wrap(self.images.uboot)
        report = InstallationReport()
        self._program(REGION_BOOT0, boot0_image)
        report.steps.append(f"boot0 programmed ({len(boot0_image)} bytes)")
        report.boot0_sha256 = hashlib.sha256(boot0_image).hexdigest()
        self._program(REGION_BOOT1, boot1_image)
        report.steps.append(f"boot1 programmed ({len(boot1_image)} bytes)")
        report.boot1_sha256 = hashlib.sha256(boot1_image).hexdigest()
        return report

    def verify(self) -> InstallationReport:
        """Read both regions back and compare against the digests."""
        report = InstallationReport()
        for region, image in (
            (REGION_BOOT0, dhtb.wrap(self.images.nandboot)),
            (REGION_BOOT1, dhtb.wrap(self.images.uboot)),
        ):
            stored = self._read(region, len(image))
            report.verify_sha256[f"0x{region:08x}"] = hashlib.sha256(stored).hexdigest()
            if stored != image:
                raise InstallError(f"{_region_label(region)} does not match the installed image")
            report.steps.append(f"{_region_label(region)} verified")
        return report

    def restore(self, backup: BootBackup) -> InstallationReport:
        """Program the saved stock bytes back, region by region."""
        report = InstallationReport(
            steps=["boot0 restored", "boot1 restored"],
            boot0_sha256=hashlib.sha256(backup.boot0.image).hexdigest(),
            boot1_sha256=hashlib.sha256(backup.boot1.image).hexdigest(),
        )
        self._program(REGION_BOOT0, backup.boot0.image)
        self._program(REGION_BOOT1, backup.boot1.image)
        return report


# The simulator stage set: what a demo "phone" boots.


def stock_boot_chain(
    boot0_payload: bytes = b"STOCK-RTOS-FDL1-PLACEHOLDER",
    boot1_payload: bytes = b"STOCK-RTOS-STAGE1-PLACEHOLDER",
    kernel_payload: bytes = b"STOCK-RTOS-KERNEL-PLACEHOLDER",
) -> tuple[bytes, bytes, bytes]:
    """Build a stock-looking boot0/boot1/kernel image set."""
    return (
        dhtb.wrap(boot0_payload),
        dhtb.wrap(boot1_payload),
        dhtb.wrap(kernel_payload),
    )


def build_test_stage_images() -> StageImages:
    """Deterministic stand-in payloads for the three built images.

    Sizes follow the real thing: the nandboot stage is small (a
    loader, not a kernel), U-Boot with its payload allocation is a few
    hundred KiB, and the flash stage is a thin command layer.
    """
    return StageImages(
        nandboot=b"\x2eFPLINUX-NANDBOOT-T117-V1" + b"\x90" * 4096,
        uboot=b"\x2eFPLINUX-UBOOT-T117-V1" + b"\x91" * (400 * 1024),
        flash_stage=b"\x2eFPLINUX-FLASHSTAGE-T117-V1" + b"\x92" * 8192,
    )


def stock_phone(images: StageImages, *, exec_distance: int = 0) -> SimulatedDevice:
    """A simulated UMS9117 with its factory chain in NAND.

    The phone carries the exec-distance quirk the target config
    documents, so the demo passes the same value to the device (what
    the BROM does) and to the client (what the host tool sends).
    """
    device = SimulatedDevice(exec_distance=exec_distance)
    stock0, stock1, stock_kernel = stock_boot_chain()
    bind_stage_actions(
        device,
        images,
        stock_boot0=dhtb.Image(stock0).payload,
        stock_boot1=dhtb.Image(stock1).payload,
        stock_kernel=dhtb.Image(stock_kernel).payload,
    )
    device.regions.program(REGION_BOOT0, stock0)
    device.regions.program(REGION_BOOT1, stock1)
    device.regions.program(REGION_KERNEL, stock_kernel)
    device.sd_card = {"FPLBOOT/FPLINUX.ITB": ITB_MAGIC.to_bytes(4, "big") + b"\x2e" * 4096}
    return device


def toolchain_demo(*, exec_distance: int = 0) -> dict[str, object]:
    """Run the entire feature against the simulator and report.

    Executable proof: a phone whose NAND boots the stock RTOS chain is
    discovered, backed up, installed, verified, booted to the Linux
    session, tampered with (the chain reports the mismatch), and
    restored to its stock boot.
    """
    images = build_test_stage_images()
    device = stock_phone(images, exec_distance=exec_distance)
    result: dict[str, object] = {"exec_distance": exec_distance}

    result["stock_boot"] = device.power_on(boot_key_held=False)
    result["download_mode"] = device.power_on(boot_key_held=True)

    installer = Installer(device.transfer, images, exec_distance=exec_distance)
    backup = installer.discover()
    result["discovered"] = {
        "chip_id": f"0x{backup.chip_id:08x}",
        "boot0_size": len(backup.boot0.image),
        "boot1_size": len(backup.boot1.image),
    }
    result["install"] = installer.install(backup).steps
    result["verify"] = installer.verify().steps
    installer.close()

    result["fplinux_boot"] = device.power_on(boot_key_held=False)

    # A corrupt boot1 payload byte: the power-on chain must stop at
    # the digest check instead of jumping into garbage.
    byte = device.nand.block_bytes + dhtb.HEADER_BYTES + 16
    device.nand.main[byte] ^= 0xFF
    result["tampered_boot"] = device.power_on(boot_key_held=False)
    device.nand.main[byte] ^= 0xFF

    # Restore runs after a power cycle, from the saved backup.
    device.power_on(boot_key_held=True)
    restorer = Installer(device.transfer, images, exec_distance=exec_distance)
    result["restore"] = restorer.restore(backup).steps
    restorer.close()
    result["restored_boot"] = device.power_on(boot_key_held=False)
    result["back_to_download_mode"] = device.power_on(boot_key_held=True)
    result["chain_log"] = list(device.chain_log)
    return result


def main(argv: list[str] | None = None) -> int:
    """The CLI surface: a documented, reproducible dry run."""
    parser = argparse.ArgumentParser(
        prog="fplinux-persistent",
        description="UMS9117 persistent-boot installer (simulated dry run)",
    )
    parser.add_argument(
        "--exec-distance",
        type=lambda value: int(value, 0),
        default=0,
        help="T117 exec-distance workaround value (target config, 0 = plain EXEC)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="print the run report as JSON",
    )
    parser.add_argument(
        "--log",
        default=None,
        help="write the full device chain log to this file",
    )
    args = parser.parse_args(argv)
    result = toolchain_demo(exec_distance=args.exec_distance)
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print("UMS9117 persistent-boot installer, simulated dry run")
        print(f"exec distance: {result['exec_distance']:#x}")
        print(f"power-on, stock chain: {result['stock_boot']}")
        print(f"power-on, boot key held: {result['download_mode']}")
        print(f"discovered: {dict(result['discovered'])}")
        for step in result["install"]:
            print(f"install: {step}")
        for step in result["verify"]:
            print(f"verify: {step}")
        print(f"power-on after install: {result['fplinux_boot']}")
        print(f"power-on after tamper: {result['tampered_boot']}")
        for step in result["restore"]:
            print(f"restore: {step}")
        print(f"power-on after restore: {result['restored_boot']}")
    if args.log:
        with open(args.log, "w", encoding="utf-8") as log_file:
            for line in result["chain_log"]:
                log_file.write(line + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

