# SPDX-License-Identifier: GPL-2.0-only
"""Exercise shared extraction with relocated parsers and complete sparse dumps."""

from __future__ import annotations

import hashlib
import json
import shutil
from dataclasses import replace
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.small.device_data.audio_expected import INOI_VIBRATE_TONE_SECTION
from tests.small.device_data.audio_fixtures import headphone_fed_records, inoi_audio_records
from tests.small.device_data.headfree_fixtures import headphone_only_headfree_records
from tests.small.device_data.nv_fixtures import nv1
from tests.small.device_data.portable_fixtures import (
    shared_extraction as shared_extraction,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from collections.abc import Callable

    from tests.small.device_data.portable_fixtures import SharedExtraction


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
