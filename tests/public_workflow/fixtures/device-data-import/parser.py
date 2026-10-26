# SPDX-License-Identifier: GPL-2.0-only
"""Expose one short radio record from a complete synthetic physical NAND image."""

from fplinux_cli.device_data.formats import (
    AudioProfileContext,
    DeviceDataPreparation,
    PhysicalNand,
    PreparedGroup,
)


def prepare_device_data(
    nand: PhysicalNand, *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Read the controlled bytes through the real physical-page consumer."""
    del audio
    radio = nand.main_bytes(0, 4)
    return DeviceDataPreparation(
        groups={
            "bluetooth": PreparedGroup(
                originals={"radio-original.bin": radio}, prepared={"radio.bin": radio}
            )
        }
    )
