# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Prepare UNIWA V1000 playback and internal radio data from its NAND backup."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.device_data.audio_profile import AUDIO_RECORD_SIZES, prepare_headset_gain_profile
from fplinux_cli.device_data.bluetooth_firmware import (
    CM4_PARTITION_ID,
    DOWNLOADED_NV_PARTITION_ID,
    NV_FILES,
    PROTECT_NV_PARTITION_ID,
    REQUIRED_BLUETOOTH_PARTITIONS,
    RUNNING_NV_PARTITION_ID,
    prepare_bluetooth_from_settings,
)
from fplinux_cli.device_data.cm4 import cm4_image
from fplinux_cli.device_data.fm_radio import FM_RECORD_SIZES, prepare_fm_config
from fplinux_cli.device_data.formats import DeviceDataPreparation, fixed_nv_records
from fplinux_cli.device_data.partitions import discover_partitions
from fplinux_cli.device_data.running_nv import read_running_nv_record

if TYPE_CHECKING:
    from fplinux_cli.device_data.formats import PhysicalNand

_RUNNING_NV_VIRTUAL_BLOCKS = 40
_PREFIX = "uniwa-v1000-4g"


def prepare_device_data(nand: PhysicalNand) -> DeviceDataPreparation:
    """Retain matching calibration and the indexed current Bluetooth identity."""
    partitions = discover_partitions(nand, REQUIRED_BLUETOOTH_PARTITIONS).extents
    required_sizes = {
        **{identifier: size for identifier, (_filename, size) in NV_FILES.items()},
        **AUDIO_RECORD_SIZES,
        **FM_RECORD_SIZES,
    }
    downloaded = fixed_nv_records(
        nand.partition_bytes(partitions[DOWNLOADED_NV_PARTITION_ID]), required_sizes
    )
    protected = fixed_nv_records(
        nand.partition_bytes(partitions[PROTECT_NV_PARTITION_ID]), required_sizes
    )
    current = {}
    for identifier, (_filename, size) in NV_FILES.items():
        if downloaded[identifier] != protected[identifier]:
            raise ValueError(f"Bluetooth NV{identifier} differs between fixed copies")
        current[identifier] = read_running_nv_record(
            nand,
            partitions[RUNNING_NV_PARTITION_ID],
            identifier=identifier,
            size=size,
            virtual_blocks=_RUNNING_NV_VIRTUAL_BLOCKS,
        )
        if identifier != 401 and current[identifier] != downloaded[identifier]:
            raise ValueError(f"current Bluetooth NV{identifier} differs from fixed calibration")
    bluetooth = prepare_bluetooth_from_settings(
        cm4_image(nand.partition_bytes(partitions[CM4_PARTITION_ID])), current, prefix=_PREFIX
    )
    audio = prepare_headset_gain_profile(
        downloaded,
        protected,
        prefix=_PREFIX,
        machine_compatible=b"uniwa,v1000-4g",
        headfree_headphones_only=True,
    )
    fm = prepare_fm_config(downloaded, protected, prefix=_PREFIX)
    return DeviceDataPreparation(
        groups={"bluetooth": bluetooth, "audio-profile": audio, "fm-radio": fm}
    )
