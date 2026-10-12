# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Prepare TA-1618 device data from the phone's partition tables."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.device_data.bluetooth_firmware import REQUIRED_BLUETOOTH_PARTITIONS
from fplinux_cli.device_data.fitted import prepare_from_partitions
from fplinux_cli.device_data.partitions import discover_partitions

if TYPE_CHECKING:
    from fplinux_cli.device_data.formats import (
        AudioProfileContext,
        DeviceDataPreparation,
        PhysicalNand,
    )


def prepare_device_data(
    nand: PhysicalNand, *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Prepare declared groups from one NAND object and one fixed-NV parse per copy."""
    return prepare_from_partitions(
        nand,
        discover_partitions(nand, REQUIRED_BLUETOOTH_PARTITIONS).extents,
        prefix="ta1618",
        audio=audio,
    )
