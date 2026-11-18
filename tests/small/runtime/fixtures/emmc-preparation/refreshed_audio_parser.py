# SPDX-License-Identifier: GPL-2.0-only
"""Controlled replacement transformation for saved eMMC originals."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.device_data.formats import DeviceDataPreparation, PreparedGroup

if TYPE_CHECKING:
    from collections.abc import Mapping

    from fplinux_cli.device_data.formats import AudioProfileContext


def prepare_from_originals(
    originals: Mapping[str, bytes], *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Produce visibly different prepared bytes while retaining the same original."""
    del audio
    return DeviceDataPreparation(
        groups={
            "audio-profile": PreparedGroup(
                originals=dict(originals),
                prepared={"audio.bin": b"refreshed:" + originals["audio.conf"]},
            )
        }
    )
