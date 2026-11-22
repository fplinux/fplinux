# SPDX-License-Identifier: GPL-2.0-only
"""Prepare Konrow headphone calibration through physical fixed NV extents."""

from __future__ import annotations

import importlib.util
import struct
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.common import ROOT
from fplinux_cli.device_data.formats import AudioProfileContext, PhysicalNand

from tests.small.device_data.audio_expected import FITTED_NOKIA_PROCESSING_SECTION
from tests.small.device_data.audio_fixtures import nokia_audio_records
from tests.small.device_data.nv_fixtures import nv1

if TYPE_CHECKING:
    from types import ModuleType

_AUDIO = AudioProfileContext(
    b"konrow,senior-280-plus",
    speaker_vibration=False,
    speaker_from_headphones=False,
    headfree_headphones_only=False,
)


@pytest.fixture(scope="module")
def konrow_parser() -> ModuleType:
    path = ROOT / "targets/konrow-senior-280-plus/device-data/konrow280plus_device_data.py"
    spec = importlib.util.spec_from_file_location("konrow280plus_device_data", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load Konrow device data parser: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _nand(*, map_sources: tuple[int, int], change_gain: bool = False) -> PhysicalNand:
    records, _protected = nokia_audio_records()
    arm = bytearray(records[426])
    struct.pack_into("<H", arm, 20, 0x0012)
    records[426] = bytes(arm)
    downloaded = nv1(tuple(records.items()))
    if change_gain:
        struct.pack_into("<H", arm, 70, 72)
        records[426] = bytes(arm)
    protected = nv1(tuple(records.items()))

    entries = struct.pack("<IHHHIHHH", 0x10000001, 0x100, 1, 1, 0x1000000F, 0x100, 2, 2)
    tables = [
        b"VBM_BOOT\x02\x01\x00\x00" + struct.pack("<6H", 5, 0, peer, 9, 8, 2) + entries
        for peer in (10, 11)
    ]
    maps = [struct.pack("<4sIHHHH", b"_MAP", 0x102, 0, 1, source, 7) for source in map_sources]
    blocks = {1: downloaded, 2: protected, 8: maps[0], 9: maps[1], 10: tables[1], 11: tables[0]}
    main = b"".join(blocks.get(block, b"").ljust(65536, b"\xff") for block in range(12))
    raw = b"".join(
        main[offset : offset + 1024] + b"\xff" * 64 for offset in range(0, len(main), 1024)
    )
    return PhysicalNand(raw, 1088, page_main_bytes=1024)


def test_matching_nv_copies_prepare_headphones_with_replacements_elsewhere(
    konrow_parser: ModuleType,
) -> None:
    prepared = konrow_parser.prepare_device_data(_nand(map_sources=(5, 5)), audio=_AUDIO)
    assert prepared.groups["audio-profile"].prepared == {
        "konrow280plus-audio-profile.bin": (
            b"FPAUDHP\0konrow,senior-280-plus\0\0"
            + bytes.fromhex("06 47 42 3d 37 32 2c 26 20 1a")
            + FITTED_NOKIA_PROCESSING_SECTION[:241]
        )
    }


@pytest.mark.parametrize(
    ("map_sources", "change_gain", "message"),
    [
        ((1, 1), False, "fixed NV depends on remapped"),
        ((4, 5), False, "replacement maps disagree"),
        ((5, 5), True, "copies disagree on headphone calibration"),
    ],
    ids=["selected-block-remapped", "different-maps", "different-calibration"],
)
def test_ambiguous_fixed_nv_is_rejected(
    konrow_parser: ModuleType, map_sources: tuple[int, int], *, change_gain: bool, message: str
) -> None:
    with pytest.raises(ValueError, match=message):
        konrow_parser.prepare_device_data(
            _nand(map_sources=map_sources, change_gain=change_gain),
            audio=_AUDIO,
        )
