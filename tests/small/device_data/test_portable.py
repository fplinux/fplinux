# SPDX-License-Identifier: GPL-2.0-only
"""Exercise shared extraction with real parsers, complete dumps and a controlled BCH child.

These host cases establish source-location independence, geometry, audio policy,
selected-sector correction and raw-marker checks. The exact-response child does
not establish the native BCH algorithm or native Windows execution.
"""

from __future__ import annotations

import hashlib
import json
import shutil
from dataclasses import dataclass, replace
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.device_data.formats import AudioProfileContext
from fplinux_cli.device_data.prepare import DeviceDataSpec, DeviceDataTools, prepare_from_nand

from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.small.device_data.audio_expected import INOI_VIBRATE_TONE_SECTION
from tests.small.device_data.audio_fixtures import headphone_fed_records, inoi_audio_records
from tests.small.device_data.headfree_fixtures import headphone_only_headfree_records
from tests.small.device_data.nv_fixtures import nv1

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path


@dataclass(frozen=True)
class SharedExtraction:
    """One relocated selected parser, complete input and caller-selected output path."""

    root: Path
    backup: Path
    spec: DeviceDataSpec
    tools: Path

    @property
    def phone(self) -> Path:
        """Return the same flat phone-store path used by checkout and archive callers."""
        return self.root / "phone"

    def prepare(self) -> Path:
        """Run the canonical shared extractor directly through its maintained interface."""
        return prepare_from_nand(
            self.spec, self.backup, self.phone, DeviceDataTools(host_tools=self.tools)
        )


@pytest.fixture
def shared_extraction(tmp_path: Path) -> SharedExtraction:
    """Own a full sparse physical image and a parser relocated outside the source checkout."""
    parser = tmp_path / "selected-parser.py"
    shutil.copyfile(
        ROOT / "tests/fixtures/portable-archive/portable_device_data_parser.py", parser
    )
    tools = tmp_path / "tools"
    tools.mkdir()
    backup = tmp_path / "nand.bin"
    with backup.open("wb") as stream:
        stream.truncate(138412032)
        stream.write(b"radio")
    spec = DeviceDataSpec(
        target="nokia-ta1618",
        preparation_recipe="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        parser=parser,
        audio=AudioProfileContext(
            machine_compatible=b"nokia,ta1618",
            speaker_vibration=False,
            speaker_from_headphones=False,
            headfree_headphones_only=False,
        ),
        nand={"id": 0x21E5, "raw_page_bytes": 2112},
        groups={
            "bluetooth": [{"source": "radio.bin", "destination": "chip/radio.bin", "size": 5}],
            "audio-profile": [
                {"source": "audio.bin", "destination": "fplinux/audio.bin", "size": 7}
            ],
            "fm-radio": [{"source": "fm.bin", "destination": "chip/fm.bin", "size": 2}],
        },
    )
    return SharedExtraction(tmp_path, backup, spec, tools)


@pytest.fixture
def shared_ecc_extraction(shared_extraction: SharedExtraction) -> SharedExtraction:
    """Use the production ECC module with one selected sector and controlled correction."""
    extraction = shared_extraction
    assert extraction.spec.parser is not None
    shutil.copyfile(
        ROOT / "tests/small/device_data/fixtures/portable/portable_ecc_parser.py",
        extraction.spec.parser,
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
    with extraction.backup.open("wb") as stream:
        stream.truncate(142606336)
        stream.write(page + b"\xff" * 2176)
    install_python_script(
        ROOT / "tests/fixtures/nand-ecc/nand_ecc_child.py", extraction.tools / "ums9117-nand-ecc"
    )
    (extraction.tools / "input.bin").write_bytes(selected_sector + b"2" * 21)
    (extraction.tools / "output.bin").write_bytes(b"b" * 100 + b"selected" + b"b" * 916)
    (extraction.tools / "exit-status.txt").write_text("0", encoding="ascii")
    (extraction.tools / "diagnostic.txt").write_text("", encoding="utf-8")
    return replace(
        extraction,
        spec=replace(
            extraction.spec,
            nand={"id": 0x81C8, "raw_page_bytes": 2176, "ecc_format": "ums9117-bch12"},
            ecc_module=ROOT / "platforms/ums9117/host/nand_ecc.py",
            groups={
                "bluetooth": [{"source": "radio.bin", "destination": "chip/radio.bin", "size": 8}]
            },
        ),
    )


@pytest.mark.parametrize("geometry_source", ["target", "receipt"])
def test_relocated_parser_publishes_all_groups_and_keeps_the_backup(
    shared_extraction: SharedExtraction, geometry_source: str
) -> None:
    extraction = shared_extraction
    with extraction.backup.open("rb") as stream:
        original_digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if geometry_source == "receipt":
        receipt = {
            "target": "nokia-ta1618",
            "acquisition_recipe": "a" * 64,
            "id_bytes": "e521",
            "chip": "controlled chip",
            "page_main_bytes": 2048,
            "oob_bytes": 64,
            "pages_per_block": 64,
            "block_count": 1024,
            "raw_bytes": 138412032,
            "sha256": original_digest,
        }
        extraction.backup.with_name("nand.bin.json").write_text(
            json.dumps(receipt), encoding="utf-8"
        )
        extraction = replace(extraction, spec=replace(extraction.spec, nand=None))

    phone = extraction.prepare()

    assert (phone / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (phone / "groups/audio-profile/audio.bin").read_bytes() == b"profile"
    assert (phone / "groups/fm-radio/fm.bin").read_bytes() == b"fm"
    assert not (phone / "source").exists()
    assert not (phone / "originals").exists()
    with extraction.backup.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == original_digest


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
def test_shared_profile_uses_selected_identity_and_audio_policy(  # noqa: PLR0913
    *,
    shared_extraction: SharedExtraction,
    policy_name: str,
    record_factory: Callable[[], tuple[dict[int, bytes], dict[int, bytes]]],
    size: int,
    expected_gains: bytes,
    expected_tone: bytes,
) -> None:
    extraction = shared_extraction
    assert extraction.spec.parser is not None
    shutil.copyfile(
        ROOT / "tests/small/device_data/fixtures/portable/portable_audio_data_parser.py",
        extraction.spec.parser,
    )
    downloaded, protected = record_factory()
    with extraction.backup.open("r+b") as stream:
        for block, records in enumerate((downloaded, protected)):
            main = nv1(tuple(sorted(records.items()))).ljust(131072, b"\xff")
            raw = b"".join(
                main[offset : offset + 2048] + b"\xff" * 64 for offset in range(0, len(main), 2048)
            )
            stream.seek(block * 135168)
            stream.write(raw)
    extraction = replace(
        extraction,
        spec=replace(
            extraction.spec,
            audio=replace(
                extraction.spec.audio,
                speaker_vibration=policy_name == "speaker_vibration",
                speaker_from_headphones=policy_name == "speaker_from_headphones",
                headfree_headphones_only=policy_name == "headfree_headphones_only",
            ),
            groups={
                "audio-profile": [
                    {
                        "source": "example-audio-profile.bin",
                        "destination": "fplinux/example-audio-profile.bin",
                        "size": size,
                    }
                ]
            },
        ),
    )

    phone = extraction.prepare()

    profile = (phone / "groups/audio-profile/example-audio-profile.bin").read_bytes()
    assert profile[:32] == b"FPAUDIO\0nokia,ta1618" + b"\0" * 12
    assert profile[32:53] == expected_gains
    assert profile[788:] == expected_tone


def test_incomplete_backup_is_rejected_before_publication(
    shared_extraction: SharedExtraction,
) -> None:
    shared_extraction.backup.write_bytes(b"incomplete NAND backup")

    with pytest.raises(
        SystemExit, match="expected a complete 138412032-byte physical NAND backup"
    ):
        shared_extraction.prepare()

    assert not shared_extraction.phone.exists()
    assert list(shared_extraction.root.glob(".phone-*")) == []


def test_declared_ecc_publishes_corrected_partition_and_keeps_raw_backup(
    shared_ecc_extraction: SharedExtraction,
) -> None:
    extraction = shared_ecc_extraction
    with extraction.backup.open("rb") as stream:
        original_digest = hashlib.file_digest(stream, "sha256").hexdigest()

    phone = extraction.prepare()

    assert (phone / "groups/bluetooth/radio.bin").read_bytes() == b"selected"
    with extraction.backup.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == original_digest
        stream.seek(1124)
        assert stream.read(8) == b"damaged!"


def test_uncorrectable_selected_sector_does_not_publish_phone_data(
    shared_ecc_extraction: SharedExtraction,
) -> None:
    extraction = shared_ecc_extraction
    (extraction.tools / "exit-status.txt").write_text("1", encoding="ascii")
    (extraction.tools / "diagnostic.txt").write_text("uncorrectable codeword\n", encoding="utf-8")

    with pytest.raises(
        SystemExit, match="NAND ECC correction at main offset 0x400: uncorrectable codeword"
    ):
        extraction.prepare()

    assert not extraction.phone.exists()
    assert list(extraction.root.glob(".phone-*")) == []


@pytest.mark.parametrize("marker", [2048, 4224], ids=["first-page", "second-page"])
def test_corrected_partition_with_raw_factory_marker_is_not_published(
    shared_ecc_extraction: SharedExtraction, marker: int
) -> None:
    extraction = shared_ecc_extraction
    with extraction.backup.open("r+b") as stream:
        stream.seek(marker)
        stream.write(b"\x00")

    with pytest.raises(SystemExit, match="selected NAND block 0 is marked bad"):
        extraction.prepare()

    assert not extraction.phone.exists()
    assert list(extraction.root.glob(".phone-*")) == []
    with extraction.backup.open("rb") as stream:
        stream.seek(marker)
        assert stream.read(1) == b"\x00"
