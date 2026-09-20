# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Select the supported INOI 244 VBM layout and fitted CM4 revision.

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

VBM_COPY_OFFSETS = (0x7FC0000, 0x7FE0000)

CM4 = Cm4Revision(
    size=463296,
    original_sha256="67dc5761280c79395fc23de33542d3eca116a39154f867a38285cdc4d174ce04",
    prepared_sha256="7fd7d006a27276c038962fb1de5ab8f3fc60f9754dc3292427caec068f9af3fd",
    pub_policy_offsets=(0x68BB4, 0x68BD0),
)


def prepare_device_data(nand: PhysicalNand) -> DeviceDataPreparation:
    """Prepare both declared groups from one NAND object and one fixed-NV parse per copy."""
    return prepare_from_partitions(
        nand,
        redundant_vbm_partitions(
            nand,
            VBM_COPY_OFFSETS,
            REQUIRED_BLUETOOTH_PARTITIONS,
        ),
        prefix="inoi244",
        revision=CM4,
        machine_compatible=b"inoi,244-modern-4g",
    )
