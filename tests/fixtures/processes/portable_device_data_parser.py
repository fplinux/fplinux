# SPDX-License-Identifier: GPL-2.0-only
"""Replace fitted phone records with controlled bytes for portable extraction cases."""

from fplinux_cli.device_data.formats import (
    AudioProfileContext,
    DeviceDataPreparation,
    PhysicalNand,
    PreparedGroup,
)


def prepare_device_data(
    nand: PhysicalNand, *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Return named groups with independently specified test payloads."""
    del audio
    radio = nand.main_bytes(0, 5)
    return DeviceDataPreparation(
        groups={
            "bluetooth": PreparedGroup(
                originals={"radio-original.bin": radio}, prepared={"radio.bin": radio}
            ),
            "audio-profile": PreparedGroup(
                originals={"audio-original.bin": b"profile"}, prepared={"audio.bin": b"profile"}
            ),
            "fm-radio": PreparedGroup(
                originals={"fm-original.bin": b"fm"}, prepared={"fm.bin": b"fm"}
            ),
        }
    )
