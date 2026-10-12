# SPDX-License-Identifier: GPL-2.0-only
"""Compose fitted device-data groups from admitted physical partitions."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.device_data.audio_profile import AUDIO_RECORD_SIZES, prepare_headset_gain_profile
from fplinux_cli.device_data.bluetooth_firmware import (
    DOWNLOADED_NV_PARTITION_ID,
    NV_FILES,
    PROTECT_NV_PARTITION_ID,
    prepare_bluetooth_from_records,
)
from fplinux_cli.device_data.fm_radio import FM_RECORD_SIZES, prepare_fm_config
from fplinux_cli.device_data.formats import (
    DeviceDataPreparation,
    PhysicalNand,
    fixed_nv_records,
)

if TYPE_CHECKING:
    from fplinux_cli.device_data.formats import AudioProfileContext


def prepare_from_partitions(
    nand: PhysicalNand,
    partitions: dict[int, tuple[int, int]],
    *,
    prefix: str,
    audio: AudioProfileContext,
) -> DeviceDataPreparation:
    """Prepare shared fitted groups from one parse of each fixed-NV copy."""
    required_sizes = {
        **{identifier: size for identifier, (_filename, size) in NV_FILES.items()},
        **AUDIO_RECORD_SIZES,
        **FM_RECORD_SIZES,
    }
    downloaded = fixed_nv_records(
        nand.partition_bytes(partitions[DOWNLOADED_NV_PARTITION_ID]),
        required_sizes,
    )
    protected = fixed_nv_records(
        nand.partition_bytes(partitions[PROTECT_NV_PARTITION_ID]),
        required_sizes,
    )
    bluetooth = prepare_bluetooth_from_records(
        nand,
        partitions,
        prefix=prefix,
        downloaded=downloaded,
        protected=protected,
    )
    audio_profile = prepare_headset_gain_profile(
        downloaded,
        protected,
        prefix=prefix,
        machine_compatible=audio.machine_compatible,
        speaker_vibration=audio.speaker_vibration,
        speaker_from_headphones=audio.speaker_from_headphones,
        headfree_headphones_only=audio.headfree_headphones_only,
    )
    fm_radio = prepare_fm_config(downloaded, protected, prefix=prefix)
    return DeviceDataPreparation(
        groups={"bluetooth": bluetooth, "audio-profile": audio_profile, "fm-radio": fm_radio}
    )
