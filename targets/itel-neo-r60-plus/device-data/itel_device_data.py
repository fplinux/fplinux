# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Prepare fitted phone data and external Marlin2 boot inputs for itel IT9310."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.common import sha256_bytes
from fplinux_cli.device_data.bluetooth_firmware import (
    DOWNLOADED_NV_PARTITION_ID,
    PROTECT_NV_PARTITION_ID,
    REQUIRED_BLUETOOTH_PARTITIONS,
)
from fplinux_cli.device_data.fitted import prepare_from_partitions
from fplinux_cli.device_data.formats import (
    DeviceDataPreparation,
    PreparedGroup,
    RequiredPartition,
    fixed_nv_records,
)
from fplinux_cli.device_data.marlin2 import RADIO_RECORD_SIZES, prepare_radio_calibration
from fplinux_cli.device_data.partitions import discover_partitions

if TYPE_CHECKING:
    from fplinux_cli.device_data.formats import AudioProfileContext, PhysicalNand

_PREFIX = "itel-neo-r60-plus"
_WCN_CODE = 0x10000014
_WCN_IMAGE_BYTES = 592896
_WCN_IMAGE_SHA256 = "453f52e204aa1211c99ef81ec665340cffff917a0da7ea84f46a18f6d8b0261e"
_WCN_IMAGE_NAME = f"{_PREFIX}-wcn.bin"
_WCN_CALIBRATION_NAME = f"{_PREFIX}-wcn-calibration.bin"


def prepare_device_data(
    nand: PhysicalNand, *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Use the existing fitted groups and the radio inputs from the same handset."""
    required = {
        **REQUIRED_BLUETOOTH_PARTITIONS,
        _WCN_CODE: RequiredPartition("External radio code", 0x100),
    }
    partitions = discover_partitions(nand, required).extents
    offset, size = partitions[_WCN_CODE]
    if size < _WCN_IMAGE_BYTES:
        message = "external radio partition is too small"
        raise ValueError(message)
    image = nand.partition_bytes((offset, _WCN_IMAGE_BYTES))
    if len(image) != _WCN_IMAGE_BYTES or sha256_bytes(image) != _WCN_IMAGE_SHA256:
        message = "external radio image does not match supported itel firmware"
        raise ValueError(message)
    downloaded = fixed_nv_records(
        nand.partition_bytes(partitions[DOWNLOADED_NV_PARTITION_ID]), RADIO_RECORD_SIZES
    )
    protected = fixed_nv_records(
        nand.partition_bytes(partitions[PROTECT_NV_PARTITION_ID]), RADIO_RECORD_SIZES
    )
    calibration = prepare_radio_calibration(downloaded, protected)
    fitted = prepare_from_partitions(nand, partitions, prefix=_PREFIX, audio=audio)
    radio = PreparedGroup(
        originals={
            _WCN_IMAGE_NAME: image,
            "nv409.bin": downloaded[409],
            "nv417.bin": downloaded[417],
        },
        prepared={_WCN_IMAGE_NAME: image, _WCN_CALIBRATION_NAME: calibration},
    )
    return DeviceDataPreparation(groups=dict(fitted.groups) | {"wcn-firmware": radio})
