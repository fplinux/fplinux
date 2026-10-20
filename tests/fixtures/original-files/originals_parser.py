# SPDX-License-Identifier: GPL-2.0-only
"""Test-owned transformation of five admitted originals into one firmware group."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.device_data.formats import DeviceDataPreparation, PreparedGroup

if TYPE_CHECKING:
    from collections.abc import Mapping


def prepare_from_originals(originals: Mapping[str, bytes]) -> DeviceDataPreparation:
    """Require a complete input set and retain its exact bytes."""
    expected = {
        "wcnmodem-firmware.bin",
        "system-connectivity_configure.ini",
        "system-connectivity_calibration.ini",
        "wcn-connectivity_configure.ini",
        "wcn-connectivity_calibration.ini",
    }
    if set(originals) != expected:
        message = "the complete five original files are required"
        raise ValueError(message)
    return DeviceDataPreparation(
        groups={
            "wcn-firmware": PreparedGroup(
                originals=dict(originals),
                prepared={
                    "wcnmodem.bin": originals["wcnmodem-firmware.bin"],
                    "system-config.bin": b"CFG:" + originals["system-connectivity_configure.ini"],
                    "system-cali.bin": b"CAL:" + originals["system-connectivity_calibration.ini"],
                    "wcn-config.bin": b"CFG:" + originals["wcn-connectivity_configure.ini"],
                    "wcn-cali.bin": b"CAL:" + originals["wcn-connectivity_calibration.ini"],
                },
            )
        }
    )
