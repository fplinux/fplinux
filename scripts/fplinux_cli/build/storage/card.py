# SPDX-License-Identifier: GPL-2.0-only
"""Produce the prebuilt microSD components installed by the RAM reader."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from fplinux_cli.alpine.rootfs_files import write_card_rootfs_layer
from fplinux_cli.artifacts.records import write_canonical_json
from fplinux_cli.artifacts.release import current_release_context
from fplinux_cli.common import fail
from fplinux_cli.runtime.card_media import CardMediaError, locate_fit_ramdisk

from . import sd

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.build.bootstrap.result import ColdSdBootstrap


def build(  # noqa: PLR0913 -- each selected source and artifact stay explicit.
    target: str,
    target_config: dict[str, Any],
    output: Path,
    *,
    board_rootfs: Path,
    shared_rootfs: Path,
    shared_ext4: Path | None,
    fit: Path | None,
    ramdisk: Path | None,
    cold_boot: ColdSdBootstrap | None,
) -> dict[str, Path]:
    """Produce the FIT, boot prefix and board layer for the installed system card."""
    if shared_ext4 is None or fit is None or ramdisk is None:
        fail("release card requires a verified ext4 root and FIT")
    if cold_boot is None:
        fail("release card requires a verified autonomous bootstrap")
    output.mkdir(parents=True, exist_ok=True)
    storage = target_config["storage"]
    prefix = output / "boot.xz"
    root = output / "root.ext4.xz"
    board = output / "board.tar"
    layout = output / "layout.json"

    try:
        ramdisk_payload = ramdisk.read_bytes()
        ramdisk_offset, hash_offset = locate_fit_ramdisk(fit.read_bytes(), ramdisk_payload)
        prefix_digest = sd.write_boot_prefix(
            shared_ext4, prefix, storage=storage, cold_boot=cold_boot
        )
        root_digest = sd.compress_card_region(shared_ext4, root, storage["root_size"])
        write_card_rootfs_layer(board_rootfs, shared_rootfs, board)
        write_canonical_json(
            layout,
            {
                "target": target,
                "release_id": current_release_context().release_id,
                "boot_offset": storage["boot_offset"],
                "boot_size": storage["boot_size"],
                "root_offset": storage["root_offset"],
                "root_size": storage["root_size"],
                "boot_sha256": prefix_digest,
                "root_sha256": root_digest,
                "fit_filename": target_config["fit"]["filename"],
                "fit_ramdisk_offset": ramdisk_offset,
                "fit_ramdisk_size": len(ramdisk_payload),
                "fit_hash_offset": hash_offset,
            },
        )
    except (OSError, CardMediaError, sd.SdImageError) as error:
        fail(str(error))

    return {
        "card/boot.xz": prefix,
        "card/root.ext4.xz": root,
        "card/board.tar": board,
        "card/fit.itb": fit,
        "card/layout.json": layout,
    }
