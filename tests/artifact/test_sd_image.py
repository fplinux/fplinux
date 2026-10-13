# SPDX-License-Identifier: GPL-2.0-only
"""Actual-artifact tests for the TA-1618 removable-media image producer."""

from __future__ import annotations

import shutil
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.build.storage import sd as sd_image
from fplinux_cli.common import sha256_file

if TYPE_CHECKING:
    from collections.abc import Iterator

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


@dataclass(frozen=True)
class SdInputs:
    """Exact FIT and ext4 inputs plus a private whole-card publication path."""

    root: Path
    fit: Path
    rootfs: Path
    output: Path

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


@pytest.fixture
def sd_inputs() -> Iterator[SdInputs]:
    """Own the large source and expanded card images for one scenario."""
    required = ("genimage", "mcopy", "mke2fs", "mkdosfs", "xz")
    missing = [name for name in required if shutil.which(name) is None]
    if missing:
        pytest.fail("quality image lacks required image tools: " + ", ".join(missing))
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        fit = root / "FPLINUX.ITB"
        rootfs = root / "FPLROOT.ext4"
        output = root / "FPLINUX.img.xz"
        fit.write_bytes(b"FPLINUX FIT test payload\n")
        with rootfs.open("wb") as stream:
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
                str(rootfs),
            ],
            check=True,
            timeout=120,
        )
        yield SdInputs(root, fit, rootfs, output)


class SdImageTests:
    """Create and inspect real MBR, FAT and ext4 artifacts without loop devices."""

    def test_builds_one_complete_mbr_image(self, sd_inputs: SdInputs) -> None:
        """A real image contains FIT in FAT p1 and the exact ext4 bytes in p2."""
        assert (sd_inputs.build()) == (sd_inputs.output)
        assert not ((sd_inputs.output.parent / "FPLINUX.img").exists())
        raw = sd_inputs.extract_raw()
        assert (raw.stat().st_size) == (ROOT_OFFSET + ROOT_SIZE)
        with raw.open("rb") as stream:
            mbr = stream.read(512)
            assert (mbr[510:512]) == (b"\x55\xaa")
            assert (struct.unpack_from("<I", mbr, 440)[0]) == (MBR_SIGNATURE)
            boot = struct.unpack_from("<B3sB3sII", mbr, 446)
            root = struct.unpack_from("<B3sB3sII", mbr, 462)
            assert ((boot[0], boot[2], boot[4], boot[5])) == ((0, 0x0C, 2048, 131072))
            assert ((root[0], root[2], root[4], root[5])) == ((0, 0x83, 133120, 131072))
            stream.seek(BOOT_OFFSET)
            boot_sector = stream.read(512)
            assert (boot_sector[71:82].rstrip()) == (b"FPLBOOT")
            assert (boot_sector[82:90]) == (b"FAT32   ")
            stream.seek(ROOT_OFFSET)
            assert (stream.read(ROOT_SIZE)) == (sd_inputs.rootfs.read_bytes())
        extracted = sd_inputs.root / "extracted.itb"
        subprocess.run(
            ["mcopy", "-i", f"{raw}@@{BOOT_OFFSET}", "::FPLINUX.ITB", str(extracted)],
            check=True,
            timeout=120,
        )
        assert (extracted.read_bytes()) == (sd_inputs.fit.read_bytes())

    def test_fit_content_is_causal(self, sd_inputs: SdInputs) -> None:
        """Changing the prebuilt FIT changes the one published image."""
        sd_inputs.build()
        before = sha256_file(sd_inputs.output)
        sd_inputs.fit.write_bytes(b"FPLINUX altered FIT payload\n")
        sd_inputs.build()
        assert (before) != (sha256_file(sd_inputs.output))

    def test_rebuild_from_unchanged_inputs_is_byte_reproducible(self, sd_inputs: SdInputs) -> None:
        """Rebuilding the same FIT and ext4 bytes publishes an identical compressed image."""
        sd_inputs.build()
        before = sha256_file(sd_inputs.output)
        sd_inputs.build()
        assert (before) == (sha256_file(sd_inputs.output))

    def test_missing_or_empty_input_is_rejected(self, sd_inputs: SdInputs) -> None:
        """An absent or empty input cannot replace a good image."""
        sd_inputs.build()
        before = sha256_file(sd_inputs.output)
        sd_inputs.fit.write_bytes(b"")
        with pytest.raises(sd_image.SdImageError, match="is empty"):
            sd_inputs.build()
        assert (before) == (sha256_file(sd_inputs.output))
        with pytest.raises(sd_image.SdImageError, match="missing or invalid"):
            sd_image.build(
                sd_inputs.root / "missing.itb",
                sd_inputs.rootfs,
                sd_inputs.output,
                fit_spec=FIT_SPEC,
                storage=STORAGE,
            )

    def test_success_replaces_tampered_output(self, sd_inputs: SdInputs) -> None:
        """A complete rebuild replaces output bytes changed after publication."""
        sd_inputs.build()
        expected = sd_inputs.output.read_bytes()
        sd_inputs.output.write_bytes(b"tampered\n")

        sd_inputs.build()

        assert (sd_inputs.output.read_bytes()) == (expected)

    def test_failed_build_preserves_prior_published_image(self, sd_inputs: SdInputs) -> None:
        """An injected genimage failure cannot replace an already complete image."""
        sd_inputs.build()
        before = sha256_file(sd_inputs.output)
        failed = subprocess.CompletedProcess(
            ["genimage"], returncode=2, stderr="injected genimage failure\n"
        )
        with (
            mock.patch("fplinux_cli.build.storage.sd.subprocess.run", return_value=failed),
            pytest.raises(sd_image.SdImageError, match="injected genimage failure"),
        ):
            sd_inputs.build()
        assert (before) == (sha256_file(sd_inputs.output))
