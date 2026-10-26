# SPDX-License-Identifier: GPL-2.0-only
"""Controlled target transform for shared source-cache and publication tests."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.device_data.formats import DeviceDataPreparation, PreparedGroup

if TYPE_CHECKING:
    from collections.abc import Mapping


def prepare_from_originals(originals: Mapping[str, bytes]) -> DeviceDataPreparation:
    """Keep the target transformation separate from the storage workflow under test."""
    if set(originals) != {"audio.conf"}:
        message = "one audio source is required"
        raise ValueError(message)
    return DeviceDataPreparation(
        groups={
            "audio-profile": PreparedGroup(
                originals=dict(originals),
                prepared={"audio.bin": b"encoded:" + originals["audio.conf"]},
            )
        }
    )
