# SPDX-License-Identifier: GPL-2.0-only
"""Target parser format selection with synthetic NAND pages and calibration."""

from __future__ import annotations

import importlib.util
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.device_data.formats import AudioProfileContext

from tests import ROOT
from tests.small.device_data.uniwa_fixtures import radio_nand

if TYPE_CHECKING:
    from types import ModuleType


@pytest.fixture
def audio_context() -> AudioProfileContext:
    """Use an explicit selected headphone-only Headfree policy."""
    return AudioProfileContext(
        machine_compatible=b"uniwa,v1000-4g",
        speaker_vibration=False,
        speaker_from_headphones=False,
        headfree_headphones_only=True,
    )


@pytest.fixture
def uniwa_parser() -> ModuleType:
    """Load the actual target parser through its normal module specification."""
    path = ROOT / "targets/uniwa-v1000-4g/uniwa_device_data.py"
    spec = importlib.util.spec_from_file_location("uniwa_data_parser", path)
    if spec is None or spec.loader is None:
        message = "target parser source is unavailable"
        raise ImportError(message)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_indexed_current_identity_is_prepared_with_matching_fixed_calibration(
    uniwa_parser: ModuleType,
    audio_context: AudioProfileContext,
) -> None:
    """An older factory address cannot replace the indexed saved Bluetooth identity."""
    result = uniwa_parser.prepare_device_data(radio_nand(), audio=audio_context)
    bluetooth = result.groups["bluetooth"]

    assert bluetooth.prepared["uniwa-v1000-4g-bt-config.bin"] == b"current!"
    assert bluetooth.originals["uniwa-v1000-4g-bt-config.bin"] == b"current!"
    assert bluetooth.prepared["uniwa-v1000-4g-bt-sprd.bin"] == b"B" * 176
    assert bluetooth.prepared["uniwa-v1000-4g-bt-rf-config.bin"] == b"C" * 252
    assert result.groups["audio-profile"].prepared["uniwa-v1000-4g-audio-profile.bin"][53:65] == (
        bytes.fromhex("1a00 06 5a 52 4a 42 3a 32 2a 22 1a")
    )


def test_saved_radio_calibration_must_match_both_fixed_copies(
    uniwa_parser: ModuleType, audio_context: AudioProfileContext
) -> None:
    """A valid current calibration record cannot silently replace fitted calibration."""
    with pytest.raises(ValueError, match="current Bluetooth NV402 differs"):
        uniwa_parser.prepare_device_data(
            radio_nand(calibration_mismatch=True), audio=audio_context
        )


def test_conflicting_fixed_calibration_is_rejected_before_radio_preparation(
    uniwa_parser: ModuleType,
    audio_context: AudioProfileContext,
) -> None:
    """Current settings do not make disagreeing protected calibration admissible."""
    with pytest.raises(ValueError, match="Bluetooth NV404 differs between fixed copies"):
        uniwa_parser.prepare_device_data(radio_nand(protected_mismatch=True), audio=audio_context)


def test_selected_identity_is_written_and_headfree_policy_is_required(
    uniwa_parser: ModuleType,
) -> None:
    """The parser uses its selected context while retaining route admission from fixed records."""
    context = AudioProfileContext(
        machine_compatible=b"example,phone",
        speaker_vibration=False,
        speaker_from_headphones=False,
        headfree_headphones_only=True,
    )

    result = uniwa_parser.prepare_device_data(radio_nand(), audio=context)

    profile = result.groups["audio-profile"].prepared["uniwa-v1000-4g-audio-profile.bin"]
    assert profile[:32] == b"FPAUDIO\0example,phone" + b"\0" * 11
    assert profile[53:65] == bytes.fromhex("1a00 06 5a 52 4a 42 3a 32 2a 22 1a")
    integrated = AudioProfileContext(
        machine_compatible=b"example,phone",
        speaker_vibration=False,
        speaker_from_headphones=False,
        headfree_headphones_only=False,
    )
    with pytest.raises(ValueError, match="Headfree does not select"):
        uniwa_parser.prepare_device_data(radio_nand(), audio=integrated)
