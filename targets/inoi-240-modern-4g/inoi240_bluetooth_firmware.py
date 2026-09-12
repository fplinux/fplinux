# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Select the supported INOI 240 VBM layout and fitted CM4 revision.

Only the matching redundant partition tables and direct physical extents are
accepted. This does not reconstruct VBM replacement blocks or recover ECC.
"""

from __future__ import annotations

from fplinux_cli.bluetooth_firmware import (
    Cm4Revision,
    FirmwarePreparation,
    PhysicalNand,
    inoi_vbm_partitions,
    prepare_from_partitions,
)

CM4 = Cm4Revision(
    size=463296,
    original_sha256="056280b2cbe34660509d54a9ce0cf516792ff38110c167f431b5ff7b77c2da0b",
    prepared_sha256="ffe5e4cb568552601281a0026bc1f9f5715f1715b1261062c23669e95b5e086d",
    pub_policy_offsets=(0x68BB4, 0x68BD0),
)


def prepare_firmware(raw: bytes) -> FirmwarePreparation:
    """Prepare the complete fitted set without modifying the physical NAND backup."""
    nand = PhysicalNand.from_dump(raw, page_bytes=2112)
    return prepare_from_partitions(nand, inoi_vbm_partitions(nand), prefix="inoi240", revision=CM4)
