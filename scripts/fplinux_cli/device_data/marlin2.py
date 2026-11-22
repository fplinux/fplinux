# SPDX-License-Identifier: GPL-2.0-only
"""Prepare calibrated Marlin2 radio inputs from fixed-NV copies."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Mapping

RADIO_RECORD_SIZES = {409: 888, 417: 3292}
_RADIO_CONFIG_BYTES = 616


def prepare_radio_calibration(
    downloaded: Mapping[int, bytes], protected: Mapping[int, bytes]
) -> bytes:
    """Retain matching fitted configuration and already calibrated radio records."""
    for identifier, size in RADIO_RECORD_SIZES.items():
        record = downloaded.get(identifier)
        if record is None or len(record) != size:
            raise ValueError(f"radio NV{identifier} is missing or has the wrong size")
        if protected.get(identifier) != record:
            raise ValueError(f"radio NV{identifier} differs between fixed copies")
    configuration = downloaded[409]
    calibration = downloaded[417]
    if configuration[0] != 2 or calibration[0] != 2:
        message = "unsupported radio configuration or calibration version"
        raise ValueError(message)
    if calibration[1] != 1:
        message = "radio record is not already calibrated"
        raise ValueError(message)
    return configuration[:_RADIO_CONFIG_BYTES] + calibration
