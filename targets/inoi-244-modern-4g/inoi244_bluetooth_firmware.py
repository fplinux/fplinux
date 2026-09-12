# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Select the supported INOI 244 VBM layout and fitted CM4 revision.

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
    original_sha256="67dc5761280c79395fc23de33542d3eca116a39154f867a38285cdc4d174ce04",
    prepared_sha256="7fd7d006a27276c038962fb1de5ab8f3fc60f9754dc3292427caec068f9af3fd",
    pub_policy_offsets=(0x68BB4, 0x68BD0),
)


def prepare_firmware(raw: bytes) -> FirmwarePreparation:
    """Prepare the complete fitted set without modifying the physical NAND backup."""
    nand = PhysicalNand.from_dump(raw, page_bytes=2112)
    return prepare_from_partitions(nand, inoi_vbm_partitions(nand), prefix="inoi244", revision=CM4)
