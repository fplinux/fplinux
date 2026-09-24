# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Select the supported INOI 240 VBM layout and fitted device data.

Only the matching redundant partition tables and direct physical extents are
accepted. This does not reconstruct VBM replacement blocks or recover ECC.
"""

from __future__ import annotations

from fplinux_cli.bluetooth_firmware import (
    REQUIRED_BLUETOOTH_PARTITIONS,
    Cm4Revision,
)
from fplinux_cli.device_data import (
    DeviceDataPreparation,
    PhysicalNand,
    redundant_vbm_partitions,
)
from fplinux_cli.fitted_device_data import prepare_from_partitions

CM4 = Cm4Revision(
    size=463296,
    original_sha256="056280b2cbe34660509d54a9ce0cf516792ff38110c167f431b5ff7b77c2da0b",
    prepared_sha256="ffe5e4cb568552601281a0026bc1f9f5715f1715b1261062c23669e95b5e086d",
    pub_policy_offsets=(0x68BB4, 0x68BD0),
)

VBM_COPY_OFFSETS = (0x7FC0000, 0x7FE0000)


def prepare_device_data(nand: PhysicalNand) -> DeviceDataPreparation:
    """Prepare declared groups from one NAND object and one fixed-NV parse per copy."""
    return prepare_from_partitions(
        nand,
        redundant_vbm_partitions(
            nand,
            VBM_COPY_OFFSETS,
            REQUIRED_BLUETOOTH_PARTITIONS,
        ),
        prefix="inoi240",
        revision=CM4,
        machine_compatible=b"inoi,240-modern-4g",
    )
