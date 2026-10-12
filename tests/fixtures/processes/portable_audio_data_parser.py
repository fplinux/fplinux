# SPDX-License-Identifier: GPL-2.0-only
"""Prepare a controlled fixed-NV audio partition through the delivered runtime."""

from fplinux_cli.device_data.audio_profile import AUDIO_RECORD_SIZES, prepare_headset_gain_profile
from fplinux_cli.device_data.formats import (
    AudioProfileContext,
    DeviceDataPreparation,
    PhysicalNand,
    fixed_nv_records,
)


def prepare_device_data(
    nand: PhysicalNand, *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Apply the selected snapshot to real record admission and profile preparation."""
    downloaded = fixed_nv_records(nand.partition_bytes((0, 131072)), AUDIO_RECORD_SIZES)
    protected = fixed_nv_records(nand.partition_bytes((131072, 131072)), AUDIO_RECORD_SIZES)
    profile = prepare_headset_gain_profile(
        downloaded,
        protected,
        prefix="example",
        machine_compatible=audio.machine_compatible,
        speaker_vibration=audio.speaker_vibration,
        speaker_from_headphones=audio.speaker_from_headphones,
        headfree_headphones_only=audio.headfree_headphones_only,
    )
    return DeviceDataPreparation(groups={"audio-profile": profile})
