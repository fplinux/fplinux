# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the fitted FM configuration carried by fixed NV record 419."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.device_data import PreparedGroup

if TYPE_CHECKING:
    from collections.abc import Mapping

FM_RECORD_SIZES = {419: 128}


def prepare_fm_config(
    downloaded: Mapping[int, bytes],
    protected: Mapping[int, bytes],
    *,
    prefix: str,
) -> PreparedGroup:
    """Apply the stock ENABLE normalization to matching fitted FM settings."""
    original = downloaded[419]
    if original != protected[419]:
        message = "FM config NV419 differs between DownloadedNV and ProtectNV"
        raise ValueError(message)

    prepared = bytearray(original)
    prepared[0:2] = b"\0\0"
    # Stock ENABLE copies th1 into th2 while retaining th1 and the reserved bytes.
    prepared[0x10:0x12] = original[0x0E:0x10]
    return PreparedGroup(
        originals={f"{prefix}-nv419.bin": original},
        prepared={f"{prefix}-fm-config.bin": bytes(prepared)},
    )
