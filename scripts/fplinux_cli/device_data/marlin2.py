# SPDX-License-Identifier: GPL-2.0-only
"""Prepare calibrated Marlin2 firmware, addresses and Bluetooth settings."""

from __future__ import annotations

from typing import TYPE_CHECKING

from .formats import PreparedGroup

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


def prepare_bluetooth_pskey(configuration: bytes, settings: bytes) -> bytes:
    """Combine admitted radio configuration and the selected eight-byte BT record."""
    if len(configuration) != 888 or configuration[0] != 2:
        message = "unsupported external Bluetooth radio configuration"
        raise ValueError(message)
    if len(settings) != 8:
        message = "saved Bluetooth record has the wrong size"
        raise ValueError(message)
    if settings[:6] == bytes(6):
        message = "saved Bluetooth address is zero"
        raise ValueError(message)
    prepared = bytearray(220)
    prepared[0x00:0x3C] = configuration[0x26C:0x2A8]
    prepared[0x3C:0x4C] = configuration[0x310:0x320]
    prepared[0x4C:0x84] = configuration[0x320:0x358]
    prepared[0x4C:0x52] = settings[:6]
    prepared[0x84:0xA4] = configuration[0x2A8:0x2C8]
    prepared[0xA4:0xC4] = configuration[0x2C8:0x2E8]
    prepared[0xC4:0xC8] = configuration[0x2E8:0x2EC]
    for index in range(8):
        source = 0x2EC + index * 4
        destination = 0xC8 + index * 2
        prepared[destination : destination + 2] = configuration[source : source + 2]
    return bytes(prepared)


def prepare_radio_group(  # noqa: PLR0913 -- image, calibration and identity are admitted separately.
    image: bytes,
    calibration: bytes,
    downloaded: Mapping[int, bytes],
    *,
    mac: bytes,
    bluetooth_settings: bytes,
    prefix: str,
) -> PreparedGroup:
    """Preserve the original records and assemble the fitted Marlin2 firmware group."""
    image_name = f"{prefix}-wcn.bin"
    pskey = prepare_bluetooth_pskey(downloaded[409], bluetooth_settings)
    return PreparedGroup(
        originals={
            image_name: image,
            "nv409.bin": downloaded[409],
            "nv417.bin": downloaded[417],
            "nv416.bin": mac,
            "nv401.bin": bluetooth_settings,
        },
        prepared={
            image_name: image,
            f"{prefix}-wcn-calibration.bin": calibration,
            f"{prefix}-wcn-mac.bin": mac,
            f"{prefix}-wcn-bluetooth.bin": pskey,
        },
    )
