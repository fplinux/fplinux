# SPDX-License-Identifier: GPL-2.0-only
"""Target parser format selection with synthetic NAND pages and calibration."""

from __future__ import annotations

import importlib.util
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.small.device_data.uniwa_fixtures import radio_nand

if TYPE_CHECKING:
    from types import ModuleType


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
) -> None:
    """An older factory address cannot replace the indexed saved Bluetooth identity."""
    result = uniwa_parser.prepare_device_data(radio_nand())
    bluetooth = result.groups["bluetooth"]

    assert bluetooth.prepared["uniwa-v1000-4g-bt-config.bin"] == b"current!"
    assert bluetooth.originals["uniwa-v1000-4g-bt-config.bin"] == b"current!"
    assert bluetooth.prepared["uniwa-v1000-4g-bt-sprd.bin"] == b"B" * 176
    assert bluetooth.prepared["uniwa-v1000-4g-bt-rf-config.bin"] == b"C" * 252
    assert result.groups["audio-profile"].prepared["uniwa-v1000-4g-audio-profile.bin"][53:65] == (
        bytes.fromhex("1a00 06 5a 52 4a 42 3a 32 2a 22 1a")
    )


def test_saved_radio_calibration_must_match_both_fixed_copies(uniwa_parser: ModuleType) -> None:
    """A valid current calibration record cannot silently replace fitted calibration."""
    with pytest.raises(ValueError, match="current Bluetooth NV402 differs"):
        uniwa_parser.prepare_device_data(radio_nand(calibration_mismatch=True))


def test_conflicting_fixed_calibration_is_rejected_before_radio_preparation(
    uniwa_parser: ModuleType,
) -> None:
    """Current settings do not make disagreeing protected calibration admissible."""
    with pytest.raises(ValueError, match="Bluetooth NV404 differs between fixed copies"):
        uniwa_parser.prepare_device_data(radio_nand(protected_mismatch=True))
