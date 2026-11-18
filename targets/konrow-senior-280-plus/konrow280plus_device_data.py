# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Prepare Konrow playback calibration from its indexed NV volume."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.device_data.audio_profile import AUDIO_RECORD_SIZES, prepare_headphone_profile
from fplinux_cli.device_data.formats import DeviceDataPreparation, PreparedGroup
from fplinux_cli.device_data.nv_volume import read_nv_volume_records

if TYPE_CHECKING:
    from collections.abc import Mapping

    from fplinux_cli.device_data.formats import AudioProfileContext


def prepare_from_originals(
    originals: Mapping[str, bytes], *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Admit one complete NV volume and retain only the declared playback output."""
    if len(originals) != 1:
        message = "Konrow preparation requires one indexed NV volume"
        raise ValueError(message)
    volume = next(iter(originals.values()))
    records = read_nv_volume_records(volume, AUDIO_RECORD_SIZES)
    profile = prepare_headphone_profile(
        records,
        prefix="konrow280plus",
        machine_compatible=audio.machine_compatible,
    )
    return DeviceDataPreparation(
        groups={
            "audio-profile": PreparedGroup(
                originals=dict(originals),
                prepared=profile.prepared,
                reports=profile.reports,
            )
        }
    )
