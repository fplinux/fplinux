# SPDX-License-Identifier: GPL-2.0-only
"""Select one controlled partition through the delivered ECC decoder boundary."""

from fplinux_cli.device_data.formats import (
    AudioProfileContext,
    DeviceDataPreparation,
    PhysicalNand,
    PreparedGroup,
)


def prepare_device_data(
    nand: PhysicalNand, *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Read the selected bytes through the real partition decoder."""
    del audio
    radio = nand.partition_bytes((1124, 8))
    return DeviceDataPreparation(
        groups={
            "bluetooth": PreparedGroup(
                originals={"radio-original.bin": radio}, prepared={"radio.bin": radio}
            ),
        }
    )
