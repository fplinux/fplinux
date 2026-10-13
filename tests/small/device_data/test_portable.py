# SPDX-License-Identifier: GPL-2.0-only
"""Exercise standalone dump extraction with controlled parser and host-tool boundaries.

The archive imports, complete-dump admission, board-map extraction, group
validation and prepared-data publication are real. A small target parser replaces
the fitted phone records, and an external fake replaces the compiled firmware
decoder. Unused dump pages are sparse; selected stock-image and partition-table
extents contain controlled bytes. ECC cases run the delivered Python projection
through an exact-response child replacing native BCH correction. These cases do
not establish fitted-data extraction, BCH correctness or native Windows execution.
"""

from __future__ import annotations

import hashlib
import json
import shutil
from types import SimpleNamespace
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.fixtures.portable_archive import (
    portable_archive as portable_archive,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.fixtures.stock_board_report import STOCK_IMAGE, agreeing_vbm, backup, signed_image
from tests.small.device_data.audio_expected import INOI_VIBRATE_TONE_SECTION
from tests.small.device_data.audio_fixtures import headphone_fed_records, inoi_audio_records
from tests.small.device_data.headfree_fixtures import headphone_only_headfree_records
from tests.small.device_data.nv_fixtures import nv1

if TYPE_CHECKING:
    from collections.abc import Callable

    from tests.fixtures.portable_archive import PortableArchive


@pytest.fixture
def portable_ecc_archive(portable_archive: PortableArchive) -> PortableArchive:
    """Deliver the real ECC projection with one selected partition and controlled BCH child."""
    archive = portable_archive
    shutil.copyfile(
        ROOT / "tests/small/device_data/fixtures/portable/portable_ecc_parser.py",
        archive.root / "targets/nokia-ta1618/device_data.py",
    )
    shutil.copyfile(
        ROOT / "platforms/ums9117/host/nand_ecc.py",
        archive.root / "platforms/ums9117/host/nand_ecc.py",
    )
    archive.manifest["nand"] = {
        "id": 0x81C8,
        "raw_page_bytes": 2176,
        "ecc_format": "ums9117-bch12",
    }
    archive.manifest["device_data"]["groups"] = {
        "bluetooth": [{"source": "radio.bin", "destination": "chip/radio.bin", "size": 8}],
    }
    (archive.root / "device-data-manifest.json").write_text(
        json.dumps(archive.manifest), encoding="utf-8"
    )
    selected_sector = b"B" * 100 + b"damaged!" + b"B" * 916
    page = (
        b"!" * 1024
        + selected_sector
        + b"\xffabc"
        + b"1" * 21
        + b"ignored"
        + b"defg"
        + b"2" * 21
        + b"outside"
        + b"Z" * 64
    )
    with archive.dump.open("wb") as stream:
        stream.truncate(142606336)
        stream.write(page + b"\xff" * 2176)
    tools = archive.root / "tools"
    install_python_script(
        ROOT / "tests/fixtures/nand-ecc/nand_ecc_child.py", tools / "ums9117-nand-ecc"
    )
    (tools / "input.bin").write_bytes(selected_sector + b"2" * 21)
    (tools / "output.bin").write_bytes(b"b" * 100 + b"selected" + b"b" * 916)
    (tools / "exit-status.txt").write_text("0", encoding="ascii")
    (tools / "diagnostic.txt").write_text("", encoding="utf-8")
    return archive


@pytest.mark.parametrize("geometry_source", ["target", "receipt"])
def test_standalone_dump_publishes_all_groups_and_keeps_the_input(
    portable_archive: PortableArchive, geometry_source: str
) -> None:
    archive = portable_archive
    with archive.dump.open("rb") as stream:
        original_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
    if geometry_source == "receipt":
        archive.manifest["nand"] = None
        (archive.root / "device-data-manifest.json").write_text(
            json.dumps(archive.manifest), encoding="utf-8"
        )
        receipt = {
            "target": "nokia-ta1618",
            "id_bytes": "e521",
            "chip": "controlled chip",
            "page_main_bytes": 2048,
            "oob_bytes": 64,
            "pages_per_block": 64,
            "block_count": 1024,
            "raw_bytes": 138412032,
            "sha256": original_sha256,
        }
        archive.dump.with_name(archive.dump.name + ".json").write_text(
            json.dumps(receipt), encoding="utf-8"
        )

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 0, result.stderr
    generation = archive.prepared_data()
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (generation / "groups/audio-profile/audio.bin").read_bytes() == b"profile"
    assert (generation / "groups/fm-radio/fm.bin").read_bytes() == b"fm"
    assert (generation / "groups/board-maps/pinmap.bin").read_bytes() == bytes.fromhex(
        "b0002a40 10000000 ffffffff ffffffff"
    )
    assert (generation / "groups/board-maps/keymap.bin").read_bytes() == b"\x2a\x00\x31\x00"
    report = json.loads((generation / "reports/board-maps/board-report.json").read_text())
    assert report["keypad"]["boot_key"] == {"code": "0x2a", "key": "FPLINUX_KEY_STAR"}
    assert not (generation / "source").exists()
    assert not (generation / "originals").exists()
    with archive.dump.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == original_sha256
    assert "Your phone data is ready in data/phone." in result.stdout
    assert not (archive.root / ".cache").exists()


def test_repeated_dump_preparation_preserves_the_selected_phone_data(
    portable_archive: PortableArchive,
) -> None:
    archive = portable_archive
    first = archive.run("--from-dump", str(archive.dump))
    assert first.returncode == 0, first.stderr
    generation = archive.prepared_data()
    with archive.dump.open("r+b") as stream:
        stream.write(b"other")

    second = archive.run("--from-dump", str(archive.dump))

    assert second.returncode == 1
    assert "prepared phone data already exists" in second.stderr
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert not list((archive.root / "data").glob(".phone-*"))


@pytest.mark.parametrize(
    ("policy_name", "record_factory", "size", "expected_gains", "expected_tone"),
    [
        (
            "speaker_vibration",
            inoi_audio_records,
            822,
            bytes.fromhex("07 6c 61 56 4b 41 37 2d 23 1b 1a00 39 35 31 2d 29 21 1d 19 17"),
            INOI_VIBRATE_TONE_SECTION,
        ),
        (
            "speaker_from_headphones",
            headphone_fed_records,
            788,
            bytes.fromhex("07 6f 66 5d 54 4b 42 39 30 27 0000 60 58 50 48 40 38 30 28 20"),
            b"",
        ),
        (
            "headfree_headphones_only",
            headphone_only_headfree_records,
            788,
            bytes.fromhex("04 5f 57 4f 47 3f 37 2f 27 1f 2b00 57 4f 47 3f 37 2f 27 1f 17"),
            b"",
        ),
    ],
)
def test_standalone_profile_uses_the_selected_identity_and_audio_policy(  # noqa: PLR0913
    *,
    portable_archive: PortableArchive,
    policy_name: str,
    record_factory: Callable[[], tuple[dict[int, bytes], dict[int, bytes]]],
    size: int,
    expected_gains: bytes,
    expected_tone: bytes,
) -> None:
    """The delivered parser consumes the manifest snapshot without a source target manifest."""
    archive = portable_archive
    shutil.copyfile(
        ROOT / "tests/small/device_data/fixtures/portable/portable_audio_data_parser.py",
        archive.root / "targets/nokia-ta1618/device_data.py",
    )
    downloaded, protected = record_factory()
    with archive.dump.open("r+b") as stream:
        for block, records in enumerate((downloaded, protected)):
            main = nv1(tuple(sorted(records.items()))).ljust(131072, b"\xff")
            raw = b"".join(
                main[offset : offset + 2048] + b"\xff" * 64 for offset in range(0, len(main), 2048)
            )
            stream.seek(block * 135168)
            stream.write(raw)
    archive.manifest["audio_policy"][policy_name] = True
    archive.manifest["device_data"]["groups"] = {
        "audio-profile": [
            {
                "source": "example-audio-profile.bin",
                "destination": "fplinux/example-audio-profile.bin",
                "size": size,
            }
        ]
    }
    (archive.root / "device-data-manifest.json").write_text(
        json.dumps(archive.manifest), encoding="utf-8"
    )

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 0, result.stderr
    profile_path = archive.prepared_data() / "groups/audio-profile/example-audio-profile.bin"
    profile = profile_path.read_bytes()
    assert profile[:32] == b"FPAUDIO\0nokia,ta1618" + b"\0" * 12
    assert profile[32:53] == expected_gains
    assert profile[788:] == expected_tone


def test_archive_cli_has_no_target_selector(portable_archive: PortableArchive) -> None:
    result = portable_archive.run(
        "--from-dump", str(portable_archive.dump), "--target", "unregistered-phone"
    )

    assert result.returncode == 2
    assert "unrecognized arguments: --target unregistered-phone" in result.stderr
    assert not (portable_archive.root / ".cache").exists()


def test_board_map_extractor_uses_windows_executable_name(
    portable_archive: PortableArchive, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Select the native basename while running the controlled decoder on the host."""
    tools = portable_archive.root / "tools"
    (tools / "fphelper_t117").rename(tools / "fphelper_t117.exe")
    monkeypatch.setattr(STOCK_IMAGE, "os", SimpleNamespace(name="nt"))
    nand = backup(signed_image(bytes(0x5000)), vbm=agreeing_vbm())

    prepared = STOCK_IMAGE.prepare_board_maps(nand, host_tools=tools)

    assert prepared.prepared["keymap.bin"] == b"\x2a\x00\x31\x00"


def test_incomplete_dump_is_rejected_before_publication(portable_archive: PortableArchive) -> None:
    portable_archive.dump.write_bytes(b"incomplete NAND backup")

    result = portable_archive.run("--from-dump", str(portable_archive.dump))

    assert result.returncode == 1
    assert "expected a complete 138412032-byte physical NAND backup" in result.stderr
    assert not (portable_archive.root / ".cache").exists()


def test_declared_ecc_publishes_corrected_partition_and_preserves_raw_backup(
    portable_ecc_archive: PortableArchive,
) -> None:
    archive = portable_ecc_archive
    with archive.dump.open("rb") as stream:
        original_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 0, result.stderr
    generation = archive.prepared_data()
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"selected"
    assert not (generation / "originals").exists()
    with archive.dump.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == original_sha256
        stream.seek(1124)
        assert stream.read(8) == b"damaged!"


def test_uncorrectable_selected_sector_does_not_publish_phone_data(
    portable_ecc_archive: PortableArchive,
) -> None:
    archive = portable_ecc_archive
    tools = archive.root / "tools"
    (tools / "exit-status.txt").write_text("1", encoding="ascii")
    (tools / "diagnostic.txt").write_text("uncorrectable codeword\n", encoding="utf-8")

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 1
    assert "NAND ECC correction at main offset 0x400: uncorrectable codeword" in result.stderr
    assert not archive.prepared_data().exists()
    assert not list((archive.root / "data").glob(".phone-*"))


@pytest.mark.parametrize("marker", [2048, 4224], ids=["first-page", "second-page"])
def test_corrected_partition_with_a_raw_factory_marker_is_not_published(
    portable_ecc_archive: PortableArchive, marker: int
) -> None:
    archive = portable_ecc_archive
    with archive.dump.open("r+b") as stream:
        stream.seek(marker)
        stream.write(b"\x00")

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 1
    assert "selected NAND block 0 is marked bad" in result.stderr
    assert not archive.prepared_data().exists()
    assert not list((archive.root / "data").glob(".phone-*"))
    with archive.dump.open("rb") as stream:
        stream.seek(marker)
        assert stream.read(1) == b"\x00"
