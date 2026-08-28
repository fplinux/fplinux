# SPDX-License-Identifier: GPL-2.0-only
"""Actual-artifact tests for the TA-1618 removable-media image producer."""

from __future__ import annotations

import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import sd_image
from fplinux_cli.common import sha256_file

BOOT_OFFSET = 1 * 1024 * 1024
BOOT_SIZE = 64 * 1024 * 1024
ROOT_OFFSET = 65 * 1024 * 1024
ROOT_SIZE = 64 * 1024 * 1024
MBR_SIGNATURE = 0x46504C58
FIT_SPEC = {"kind": "sha256", "filename": "FPLINUX.ITB"}
STORAGE = {
    "filename": "FPLINUX.img",
    "disk_signature": MBR_SIGNATURE,
    "boot_partition": 1,
    "boot_offset": BOOT_OFFSET,
    "boot_size": BOOT_SIZE,
    "boot_label": "FPLBOOT",
    "root_partition": 2,
    "root_offset": ROOT_OFFSET,
    "root_size": ROOT_SIZE,
    "root_filename": "FPLROOT.ext4",
    "root_label": "FPLROOT",
    "root_uuid": "042681b5-d000-5b78-9c16-8e8b2944594e",
    "partuuid": "46504c58-02",
    "block_size": 4096,
    "inode_size": 256,
}


class SdImageTests(unittest.TestCase):
    """Create and inspect real MBR, FAT and ext4 artifacts without loop devices."""

    def setUp(self) -> None:
        """Create exact FIT and ext4 inputs for one whole-card image."""
        required = ("genimage", "mcopy", "mke2fs", "mkdosfs", "xz")
        missing = [name for name in required if shutil.which(name) is None]
        if missing:
            self.skipTest("required image tools are unavailable: " + ", ".join(missing))
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fit = self.root / "FPLINUX.ITB"
        self.rootfs = self.root / "FPLROOT.ext4"
        self.output = self.root / "FPLINUX.img.xz"
        self.fit.write_bytes(b"FPLINUX FIT test payload\n")
        with self.rootfs.open("wb") as stream:
            stream.truncate(ROOT_SIZE)
        subprocess.run(
            [
                "mke2fs",
                "-q",
                "-F",
                "-t",
                "ext4",
                "-U",
                "042681b5-d000-5b78-9c16-8e8b2944594e",
                "-L",
                "FPLROOT",
                str(self.rootfs),
            ],
            check=True,
            timeout=120,
        )

    def build(self) -> Path:
        """Build from the current exact fixture inputs."""
        return sd_image.build(
            self.fit,
            self.rootfs,
            self.output,
            fit_spec=FIT_SPEC,
            storage=STORAGE,
        )

    def extract_raw(self) -> Path:
        """Expand the published image without making it another producer output."""
        raw = self.root / "FPLINUX.img"
        with raw.open("wb") as stream:
            subprocess.run(
                ["xz", "-dc", str(self.output)],
                stdout=stream,
                check=True,
                timeout=120,
            )
        return raw

    def test_builds_one_complete_mbr_image(self) -> None:
        """A real image contains FIT in FAT p1 and the exact ext4 bytes in p2."""
        self.assertEqual(self.build(), self.output)
        self.assertFalse((self.output.parent / "FPLINUX.img").exists())
        raw = self.extract_raw()
        self.assertEqual(raw.stat().st_size, ROOT_OFFSET + ROOT_SIZE)
        with raw.open("rb") as stream:
            mbr = stream.read(512)
            self.assertEqual(mbr[510:512], b"\x55\xaa")
            self.assertEqual(struct.unpack_from("<I", mbr, 440)[0], MBR_SIGNATURE)
            boot = struct.unpack_from("<B3sB3sII", mbr, 446)
            root = struct.unpack_from("<B3sB3sII", mbr, 462)
            self.assertEqual((boot[0], boot[2], boot[4], boot[5]), (0, 0x0C, 2048, 131072))
            self.assertEqual((root[0], root[2], root[4], root[5]), (0, 0x83, 133120, 131072))
            stream.seek(BOOT_OFFSET)
            boot_sector = stream.read(512)
            self.assertEqual(boot_sector[71:82].rstrip(), b"FPLBOOT")
            self.assertEqual(boot_sector[82:90], b"FAT32   ")
            stream.seek(ROOT_OFFSET)
            self.assertEqual(stream.read(ROOT_SIZE), self.rootfs.read_bytes())
        extracted = self.root / "extracted.itb"
        subprocess.run(
            ["mcopy", "-i", f"{raw}@@{BOOT_OFFSET}", "::FPLINUX.ITB", str(extracted)],
            check=True,
            timeout=120,
        )
        self.assertEqual(extracted.read_bytes(), self.fit.read_bytes())

    def test_fit_content_is_causal(self) -> None:
        """Changing the prebuilt FIT changes the one published image."""
        self.build()
        before = sha256_file(self.output)
        self.fit.write_bytes(b"FPLINUX altered FIT payload\n")
        self.build()
        self.assertNotEqual(before, sha256_file(self.output))

    def test_unrelated_temporary_file_does_not_change_output(self) -> None:
        """Only the declared FIT and ext4 bytes affect the image."""
        self.build()
        before = sha256_file(self.output)
        (self.root / "unrelated.tmp").write_bytes(b"unrelated\n")
        self.build()
        self.assertEqual(before, sha256_file(self.output))

    def test_missing_or_empty_input_is_rejected(self) -> None:
        """An absent or empty input cannot replace a good image."""
        self.build()
        before = sha256_file(self.output)
        self.fit.write_bytes(b"")
        with self.assertRaisesRegex(sd_image.SdImageError, "is empty"):
            self.build()
        self.assertEqual(before, sha256_file(self.output))
        with self.assertRaisesRegex(sd_image.SdImageError, "missing or invalid"):
            sd_image.build(
                self.root / "missing.itb",
                self.rootfs,
                self.output,
                fit_spec=FIT_SPEC,
                storage=STORAGE,
            )

    def test_success_replaces_tampered_output(self) -> None:
        """A complete rebuild replaces output bytes changed after publication."""
        self.build()
        expected = self.output.read_bytes()
        self.output.write_bytes(b"tampered\n")

        self.build()

        self.assertEqual(self.output.read_bytes(), expected)

    def test_failed_build_preserves_prior_published_image(self) -> None:
        """A real genimage failure cannot replace an already complete image."""
        self.build()
        before = sha256_file(self.output)
        failed = subprocess.CompletedProcess(
            ["genimage"], returncode=2, stderr="injected genimage failure\n"
        )
        with (
            mock.patch("fplinux_cli.sd_image.subprocess.run", return_value=failed),
            self.assertRaisesRegex(sd_image.SdImageError, "injected genimage failure"),
        ):
            self.build()
        self.assertEqual(before, sha256_file(self.output))


if __name__ == "__main__":
    unittest.main()
