# SPDX-License-Identifier: GPL-2.0-only
"""Compose fitted device-data groups from admitted physical partitions."""

from __future__ import annotations

from fplinux_cli.audio_profile import (
    AUDIO_RECORD_SIZES,
    prepare_headset_gain_profile,
)
from fplinux_cli.bluetooth_firmware import (
    DOWNLOADED_NV_PARTITION_ID,
    NV_FILES,
    PROTECT_NV_PARTITION_ID,
    Cm4Revision,
    prepare_bluetooth_from_records,
)
from fplinux_cli.device_data import (
    DeviceDataPreparation,
    PhysicalNand,
    fixed_nv_records,
)
from fplinux_cli.fm_radio import FM_RECORD_SIZES, prepare_fm_config


def prepare_from_partitions(  # noqa: PLR0913 -- target identity and policy stay explicit.
    nand: PhysicalNand,
    partitions: dict[int, tuple[int, int]],
    *,
    prefix: str,
    revision: Cm4Revision,
    machine_compatible: bytes,
    speaker_vibration: bool = False,
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
        revision=revision,
        downloaded=downloaded,
        protected=protected,
    )
    audio_profile = prepare_headset_gain_profile(
        downloaded,
        protected,
        prefix=prefix,
        machine_compatible=machine_compatible,
        speaker_vibration=speaker_vibration,
    )
    fm_radio = prepare_fm_config(downloaded, protected, prefix=prefix)
    return DeviceDataPreparation(
        groups={"bluetooth": bluetooth, "audio-profile": audio_profile, "fm-radio": fm_radio}
    )
