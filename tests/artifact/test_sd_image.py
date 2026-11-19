# SPDX-License-Identifier: GPL-2.0-only
"""Actual-artifact tests for the TA-1618 removable-media image producer."""

from __future__ import annotations

import shutil
import struct
import subprocess
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.alpine.rootfs_files import write_card_rootfs_layer
from fplinux_cli.build.bootstrap.result import ColdSdBootstrap
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

    def build(self, cold_boot: ColdSdBootstrap | None = None) -> Path:
        """Build from the current exact fixture inputs."""
        return sd_image.build(
            self.fit,
            self.rootfs,
            self.output,
            fit_spec=FIT_SPEC,
            storage=STORAGE,
            cold_boot=cold_boot,
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
def sd_files() -> Iterator[SdInputs]:
    """Own regular input files and promptly release any large image outputs."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        fit = root / "FPLINUX.ITB"
        rootfs = root / "FPLROOT.ext4"
        output = root / "FPLINUX.img.xz"
        fit.write_bytes(b"FPLINUX FIT test payload\n")
        with rootfs.open("wb") as stream:
            stream.truncate(ROOT_SIZE)
        yield SdInputs(root, fit, rootfs, output)


@pytest.fixture
def sd_inputs(sd_files: SdInputs) -> SdInputs:
    """Format a real ext4 input for cases that produce or inspect a card image."""
    required = ("genimage", "mcopy", "mdir", "mke2fs", "mkdosfs", "xz")
    missing = [name for name in required if shutil.which(name) is None]
    if missing:
        pytest.fail("quality image lacks required image tools: " + ", ".join(missing))
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
            str(sd_files.rootfs),
        ],
        check=True,
        timeout=120,
    )
    return sd_files


class SdImageTests:
    """Create and inspect real MBR, FAT and ext4 artifacts without loop devices."""

    @pytest.mark.parametrize("autonomous", [False, True], ids=["pc-only", "with-card-entry"])
    def test_builds_one_complete_mbr_image(self, sd_inputs: SdInputs, *, autonomous: bool) -> None:
        """Both startup options retain the same partition layout and system bytes."""
        cold_boot = None
        reader_bytes = bytes(range(256)) * 3
        stage0_bytes = b"independent resident-stage fixture\n"
        if autonomous:
            reader = sd_inputs.root / "reader.bin"
            stage0 = sd_inputs.root / "resident.bin"
            reader.write_bytes(reader_bytes)
            stage0.write_bytes(stage0_bytes)
            cold_boot = ColdSdBootstrap(reader, stage0)
        assert sd_inputs.build(cold_boot) == sd_inputs.output
        assert not ((sd_inputs.output.parent / "FPLINUX.img").exists())
        raw = sd_inputs.extract_raw()
        assert (raw.stat().st_size) == (ROOT_OFFSET + ROOT_SIZE)
        with raw.open("rb") as stream:
            mbr = stream.read(512)
            assert (mbr[510:512]) == (b"\x55\xaa")
            assert (struct.unpack_from("<I", mbr, 440)[0]) == (MBR_SIGNATURE)
            if autonomous:
                assert mbr[:440] == reader_bytes[:440]
                assert stream.read(len(reader_bytes) - 512) == reader_bytes[512:]
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
        if autonomous:
            extracted_stage0 = sd_inputs.root / "extracted-stage0.bin"
            subprocess.run(
                [
                    "mcopy",
                    "-i",
                    f"{raw}@@{BOOT_OFFSET}",
                    "::fpbin_t117/fpmain.bin",
                    str(extracted_stage0),
                ],
                check=True,
                timeout=120,
            )
            assert extracted_stage0.read_bytes() == stage0_bytes

    def test_fit_content_is_causal(self, sd_inputs: SdInputs) -> None:
        """Changed FIT bytes replace the old image with the exact fresh-build result."""
        sd_inputs.build()
        before = sha256_file(sd_inputs.output)
        sd_inputs.fit.write_bytes(b"FPLINUX altered FIT payload\n")
        sd_inputs.build()
        assert (before) != (sha256_file(sd_inputs.output))
        reference = sd_inputs.root / "changed-fit-reference" / "FPLINUX.img.xz"
        assert not reference.parent.exists()
        sd_image.build(
            sd_inputs.fit,
            sd_inputs.rootfs,
            reference,
            fit_spec=FIT_SPEC,
            storage=STORAGE,
        )
        assert sd_inputs.output.read_bytes() == reference.read_bytes()

    def test_ram_linux_profile_contains_exact_ext4_root_and_an_empty_fat_partition(
        self, sd_inputs: SdInputs
    ) -> None:
        """A USB-loaded kernel receives the same card layout without a card boot payload."""
        result = sd_image.build(
            None, sd_inputs.rootfs, sd_inputs.output, fit_spec=None, storage=STORAGE
        )
        assert result == sd_inputs.output
        raw = sd_inputs.extract_raw()
        assert raw.stat().st_size == ROOT_OFFSET + ROOT_SIZE
        with raw.open("rb") as stream:
            mbr = stream.read(512)
            assert mbr[510:512] == b"\x55\xaa"
            assert struct.unpack_from("<I", mbr, 440)[0] == 0x46504C58
            assert struct.unpack_from("<B3sB3sII", mbr, 462)[4:] == (133120, 131072)
            stream.seek(ROOT_OFFSET)
            assert stream.read(ROOT_SIZE) == sd_inputs.rootfs.read_bytes()
        directory = subprocess.run(
            ["mdir", "-b", "-i", f"{raw}@@{BOOT_OFFSET}", "::"],
            capture_output=True,
            text=True,
            check=True,
            timeout=120,
        )
        assert directory.stdout == ""

    def test_card_prefix_contains_cold_entry_and_leaves_fit_absent(
        self, sd_inputs: SdInputs
    ) -> None:
        """A staged full card layout cannot boot before the final FIT is installed."""
        reader = sd_inputs.root / "reader.bin"
        stage0 = sd_inputs.root / "resident.bin"
        reader_payload = bytes(range(256)) * 3
        stage0_payload = b"independent card-resident entry\n"
        reader.write_bytes(reader_payload)
        stage0.write_bytes(stage0_payload)
        prefix = sd_inputs.root / "boot.xz"
        digest = sd_image.write_boot_prefix(
            sd_inputs.rootfs,
            prefix,
            storage=STORAGE,
            cold_boot=ColdSdBootstrap(reader, stage0),
        )
        raw = sd_inputs.root / "boot.raw"
        with raw.open("wb") as stream:
            subprocess.run(["xz", "-dc", str(prefix)], stdout=stream, check=True, timeout=120)
        assert raw.stat().st_size == 68157440
        assert digest == sha256_file(raw)
        with raw.open("rb") as stream:
            mbr = stream.read(512)
            assert mbr[:440] == reader_payload[:440]
            assert mbr[510:512] == b"\x55\xaa"
            assert struct.unpack_from("<II", mbr, 470) == (133120, 131072)
            assert stream.read(256) == reader_payload[512:]
        fat = f"{raw}@@{BOOT_OFFSET}"
        directory = subprocess.run(
            ["mdir", "-b", "-i", fat, "::"], capture_output=True, check=True, timeout=120
        )
        assert b"fplinux.itb" not in directory.stdout.lower()
        extracted = sd_inputs.root / "checked-resident.bin"
        subprocess.run(
            ["mcopy", "-i", fat, "::fpbin_t117/fpmain.bin", str(extracted)],
            check=True,
            timeout=120,
        )
        assert extracted.read_bytes() == stage0_payload

    @pytest.mark.parametrize("with_fit", [False, True], ids=["missing-fit", "unexpected-fit"])
    def test_fit_bytes_and_specification_must_be_selected_together(
        self, sd_files: SdInputs, *, with_fit: bool
    ) -> None:
        """A profile mismatch fails before replacing the previous output bytes."""
        before = b"previous published output\n"
        sd_files.output.write_bytes(before)
        with pytest.raises(sd_image.SdImageError, match="selected together"):
            sd_image.build(
                sd_files.fit if with_fit else None,
                sd_files.rootfs,
                sd_files.output,
                fit_spec=None if with_fit else FIT_SPEC,
                storage=STORAGE,
            )
        assert sd_files.output.read_bytes() == before

    def test_rebuild_from_unchanged_inputs_is_byte_reproducible(self, sd_inputs: SdInputs) -> None:
        """Rebuilding the same FIT and ext4 bytes publishes an identical compressed image."""
        sd_inputs.build()
        before = sha256_file(sd_inputs.output)
        second = sd_inputs.root / "second-build" / "FPLINUX.img.xz"
        assert not second.parent.exists()
        sd_image.build(
            sd_inputs.fit,
            sd_inputs.rootfs,
            second,
            fit_spec=FIT_SPEC,
            storage=STORAGE,
        )
        assert before == sha256_file(second)

    def test_missing_or_empty_input_is_rejected(self, sd_files: SdInputs) -> None:
        """An absent or empty input cannot replace previous output bytes."""
        before = b"previous published output\n"
        sd_files.output.write_bytes(before)
        sd_files.fit.write_bytes(b"")
        with pytest.raises(sd_image.SdImageError, match="is empty"):
            sd_files.build()
        assert sd_files.output.read_bytes() == before
        with pytest.raises(sd_image.SdImageError, match="missing or invalid"):
            sd_image.build(
                sd_files.root / "missing.itb",
                sd_files.rootfs,
                sd_files.output,
                fit_spec=FIT_SPEC,
                storage=STORAGE,
            )
        assert sd_files.output.read_bytes() == before

    def test_failed_build_preserves_previous_output(self, sd_files: SdInputs) -> None:
        """An injected genimage failure cannot replace previous output bytes."""
        before = b"previous published output\n"
        sd_files.output.write_bytes(before)
        failed = subprocess.CompletedProcess(
            ["genimage"], returncode=2, stderr="injected genimage failure\n"
        )
        with (
            mock.patch("fplinux_cli.build.storage.sd.subprocess.run", return_value=failed),
            pytest.raises(sd_image.SdImageError, match="injected genimage failure"),
        ):
            sd_files.build()
        assert sd_files.output.read_bytes() == before


def test_card_board_archive_applies_additions_and_replacements(tmp_path: Path) -> None:
    """Apply the board tar to a shared tree with an independent archive extractor."""
    base = tmp_path / "base"
    target = tmp_path / "target"
    for root in (base, target):
        (root / "etc").mkdir(parents=True)
        (root / "etc" / "common").write_bytes(b"common data\n")
    (base / "etc" / "world").write_bytes(b"common packages\n")
    (target / "etc" / "world").write_bytes(b"common and board packages\n")
    (target / "etc" / "brightness").write_bytes(b"board brightness levels\n")
    (target / "etc" / "brightness").chmod(0o640)

    base_cpio = tmp_path / "base.cpio"
    target_cpio = tmp_path / "target.cpio"
    for root, archive in ((base, base_cpio), (target, target_cpio)):
        entries = [str(path.relative_to(root)) for path in sorted(root.rglob("*"))]
        with archive.open("wb") as stream:
            subprocess.run(
                ["cpio", "-o", "-H", "newc", "--quiet"],
                input=("\n".join(entries) + "\n").encode(),
                stdout=stream,
                cwd=root,
                check=True,
                timeout=20,
            )

    board = tmp_path / "board.tar"
    write_card_rootfs_layer(target_cpio, base_cpio, board)
    with tarfile.open(board) as board_archive:
        files = {
            Path(member.name).as_posix(): member for member in board_archive if member.isfile()
        }
    assert set(files) == {"etc/brightness", "etc/world"}
    assert files["etc/brightness"].mode == 0o640
    assert files["etc/brightness"].uid == files["etc/brightness"].gid == 0

    subprocess.run(["tar", "-xf", str(board)], cwd=base, check=True, timeout=20)
    assert (base / "etc" / "common").read_bytes() == b"common data\n"
    assert (base / "etc" / "world").read_bytes() == b"common and board packages\n"
    assert (base / "etc" / "brightness").read_bytes() == b"board brightness levels\n"
