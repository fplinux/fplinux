# SPDX-License-Identifier: GPL-2.0-only
"""Check portable personalization against the FIT and tar read boundaries."""

from __future__ import annotations

import hashlib
import json
import lzma
import random
import tarfile
from io import BytesIO
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.artifacts.card_kit import CardKitError, locate_fit_ramdisk, read_card_layout
from fplinux_cli.artifacts.newc import newc_entries
from fplinux_cli.build.device_tree import exact_path_properties
from fplinux_cli.device_data.inputs import FirmwareInput
from fplinux_cli.runtime.card_media import prepare_card_media

from tests.fdt import binary_tree

if TYPE_CHECKING:
    from pathlib import Path

_BOARD_FILE = "./etc/fplinux/brightness"
_BOARD_DATA = b"display=8\n"
_FIT_BOOTSTRAP = b"fixed initramfs prefix".ljust(1024, b"\0")


@pytest.fixture
def card_kit(tmp_path: Path) -> Path:
    """Supply a fixed empty newc archive inside an independently encoded FIT."""
    card_dir = tmp_path / "card"
    card_dir.mkdir()
    with tarfile.open(card_dir / "board.tar", "x", format=tarfile.USTAR_FORMAT) as board:
        directory = tarfile.TarInfo("./etc/fplinux")
        directory.type = tarfile.DIRTYPE
        board.addfile(directory)
        board_file = tarfile.TarInfo(_BOARD_FILE)
        board_file.size = len(_BOARD_DATA)
        board.addfile(board_file, BytesIO(_BOARD_DATA))
    trailer = (
        b"070701"
        + b"00000000" * 4
        + b"00000001"
        + b"00000000" * 6
        + b"0000000b00000000"
        + b"TRAILER!!!\0"
    )
    ramdisk = _FIT_BOOTSTRAP + trailer.ljust(4096, b"\0")
    digest = hashlib.sha256(ramdisk).digest()
    fit = binary_tree(
        [("description", b"FPLinux release\0")],
        [
            (
                "images",
                [],
                [
                    ("kernel", [("data", b"test kernel payload")], []),
                    ("fdt", [("data", b"test board tree")], []),
                    ("ramdisk", [("data", ramdisk)], [("hash", [("value", digest)], [])]),
                ],
            ),
            ("configurations", [("default", b"inoi-240-modern-4g\0")], []),
        ],
    )
    (card_dir / "fit.itb").write_bytes(fit)
    layout = {
        "target": "inoi-240-modern-4g",
        "release_id": "test-release",
        "boot_offset": 1_048_576,
        "disk_signature": 0x46504C58,
        "boot_size": 67_108_864,
        "root_offset": 68_157_440,
        "root_size": 67_108_864,
        "boot_sha256": "a" * 64,
        "root_sha256": "b" * 64,
        "fit_filename": "FPLINUX.ITB",
        "fit_ramdisk_offset": fit.index(ramdisk),
        "fit_ramdisk_size": 5120,
        "fit_hash_offset": fit.index(digest),
    }
    (card_dir / "layout.json").write_text(json.dumps(layout), encoding="utf-8")
    return card_dir


def _firmware(destination: str, data: bytes) -> FirmwareInput:
    return FirmwareInput(destination, destination, data, hashlib.sha256(data).hexdigest())


@pytest.mark.parametrize("signature", [None, True, -1, 0x100000000])
def test_card_layout_requires_an_unsigned_mbr_disk_signature(
    card_kit: Path, signature: int | None
) -> None:
    layout_path = card_kit / "layout.json"
    record = json.loads(layout_path.read_text(encoding="utf-8"))
    if signature is None:
        del record["disk_signature"]
    else:
        record["disk_signature"] = signature
    layout_path.write_text(json.dumps(record), encoding="utf-8")
    with pytest.raises(CardKitError, match="invalid disk_signature"):
        read_card_layout(card_kit)


def test_personalization_updates_only_fit_ramdisk_and_installs_private_firmware(
    card_kit: Path, tmp_path: Path
) -> None:
    template = (card_kit / "fit.itb").read_bytes()
    layout = read_card_layout(card_kit)
    stage = tmp_path / "stage"
    stage.mkdir()
    modem = b"test modem firmware\x00" * 512
    groups = {
        "audio-profile": (_firmware("fplinux/audio.bin", b"sound settings"),),
        "bluetooth": (_firmware("chip/bt.bin", b"controller"),),
        "fm-radio": (_firmware("fplinux/fm.bin", b"FM settings"),),
        "wcn-firmware": (_firmware("chip/modem.bin", modem),),
        "board-maps": (_firmware("board.bin", b"board settings"),),
    }

    personal, checksums, fit = prepare_card_media(card_kit, stage, layout, groups)

    assert layout.target == "inoi-240-modern-4g"
    assert layout.release_id == "test-release"
    assert layout.capacity_bytes == 135_266_304
    assert (personal.name, checksums.name, fit.name) == (
        "personal.tar",
        "installed.sha256",
        "fit.itb",
    )
    fit_bytes = fit.read_bytes()
    assert len(fit_bytes) == len(template)
    assert (card_kit / "fit.itb").read_bytes() == template
    paths = (
        "/images/kernel",
        "/images/fdt",
        "/images/ramdisk",
        "/images/ramdisk/hash",
        "/configurations",
    )
    before = exact_path_properties(template, paths)
    after = exact_path_properties(fit_bytes, paths)
    for path in ("/images/kernel", "/images/fdt", "/configurations"):
        assert after[path] == before[path]
    ramdisk = after["/images/ramdisk"]["data"]
    assert ramdisk[:1024] == _FIT_BOOTSTRAP
    assert len(ramdisk) == 5120
    assert after["/images/ramdisk/hash"]["value"] == hashlib.sha256(ramdisk).digest()
    assert locate_fit_ramdisk(template, before["/images/ramdisk"]["data"]) == (
        template.index(before["/images/ramdisk"]["data"]),
        template.index(before["/images/ramdisk/hash"]["value"]),
    )
    early_files = {
        item.name: (item.mode, item.data) for item in newc_entries(ramdisk[-4096:]) if item.data
    }
    assert early_files == {"lib/firmware/fplinux/audio.bin": (0o100600, b"sound settings")}
    expected = {
        "lib/firmware/chip/bt.bin": b"controller",
        "lib/firmware/chip/modem.bin.xz": modem,
        "lib/firmware/fplinux/fm.bin": b"FM settings",
    }
    with tarfile.open(personal, "r") as archive:
        assert set(archive.getnames()) == expected.keys()
        lines = [f"{hashlib.sha256(_BOARD_DATA).hexdigest()}  system/a/root/{_BOARD_FILE}\n"]
        for member in archive:
            assert member.isfile()
            assert (member.mode, member.uid, member.gid) == (0o600, 0, 0)
            stored = archive.extractfile(member)
            assert stored is not None
            contents = stored.read()
            data = lzma.decompress(contents) if member.name.endswith(".xz") else contents
            assert data == expected[member.name]
            lines.append(f"{hashlib.sha256(contents).hexdigest()}  data/{member.name}\n")
    assert checksums.read_bytes() == "".join(lines).encode("ascii")


def test_card_without_phone_firmware_retains_the_fit_and_checks_only_board_files(
    card_kit: Path, tmp_path: Path
) -> None:
    stage = tmp_path / "stage"
    stage.mkdir()

    personal, checksums, fit = prepare_card_media(card_kit, stage, read_card_layout(card_kit), {})

    assert fit.read_bytes() == (card_kit / "fit.itb").read_bytes()
    assert (
        checksums.read_bytes()
        == f"{hashlib.sha256(_BOARD_DATA).hexdigest()}  system/a/root/{_BOARD_FILE}\n".encode()
    )
    with tarfile.open(personal, "r") as archive:
        assert archive.getnames() == []


@pytest.mark.parametrize("damaged_field", ["initramfs", "hash"])
def test_fit_hash_mismatch_preserves_release_and_does_not_stage_phone_data(
    card_kit: Path, tmp_path: Path, damaged_field: str
) -> None:
    layout = read_card_layout(card_kit)
    fit = card_kit / "fit.itb"
    damaged = bytearray(fit.read_bytes())
    offset = layout.fit_ramdisk_offset if damaged_field == "initramfs" else layout.fit_hash_offset
    damaged[offset] ^= 0xFF
    fit.write_bytes(damaged)
    stage = tmp_path / "stage"
    stage.mkdir()

    with pytest.raises(CardKitError, match="Invalid inline ramdisk or SHA-256"):
        prepare_card_media(card_kit, stage, layout, {})

    assert list(stage.iterdir()) == []
    assert fit.read_bytes() == damaged


def test_audio_overflow_leaves_the_release_slot_and_staging_unchanged(
    card_kit: Path, tmp_path: Path
) -> None:
    layout = read_card_layout(card_kit)
    template = (card_kit / "fit.itb").read_bytes()
    stage = tmp_path / "stage"
    stage.mkdir()
    audio = _firmware("fplinux/profile.bin", random.Random(867).randbytes(5000))  # noqa: S311 -- fixture.

    with pytest.raises(CardKitError, match="exceeds the release FIT ramdisk slot"):
        prepare_card_media(card_kit, stage, layout, {"audio-profile": (audio,)})

    assert list(stage.iterdir()) == []
    assert (card_kit / "fit.itb").read_bytes() == template
