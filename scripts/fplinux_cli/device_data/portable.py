# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the archive's declared device data using its bundled host tool."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path
from typing import Any

from fplinux_cli.common import ROOT, fail, read_json_object
from fplinux_cli.device_data.formats import PhysicalNand
from fplinux_cli.device_data.prepare import (
    BOARD_MAPS_GROUP,
    _board_maps_extractor,
    _extract_groups,
    _nand_main_decoder,
    _read_dump,
    dump_page_bytes,
    materialize_device_data,
)
from fplinux_cli.manifests.values import basename_value, exact_table
from fplinux_cli.runtime.nand_backup import read_backup_geometry


def prepare_portable_device_data(*, from_dump: Path, destination: Path) -> Path:
    """Extract and select all groups for the single target recorded in the archive."""
    manifest = exact_table(
        read_json_object(ROOT / "device-data-manifest.json"),
        {"target", "platform", "nand", "device_data"},
        "archive device-data manifest",
    )
    target = basename_value(manifest["target"], "archive target")
    platform = basename_value(manifest["platform"], "archive platform")
    device_data = exact_table(manifest["device_data"], {"parser", "groups"}, "archive device_data")
    declarations: dict[str, list[dict[str, Any]]] = device_data["groups"]
    if not declarations:
        fail(f"device-data preparation is not supported for target {target}")

    if destination.exists() or destination.is_symlink():
        fail(f"prepared phone data already exists: {destination}")
    raw = _read_dump(from_dump)
    nand_declaration = manifest["nand"]
    page_bytes = dump_page_bytes(target, nand_declaration, read_backup_geometry(from_dump, raw))
    main_decoder = (
        _nand_main_decoder(platform, host_tools=ROOT / "tools")
        if nand_declaration is not None and "ecc_format" in nand_declaration
        else None
    )
    try:
        nand = PhysicalNand.from_dump(raw, page_bytes=page_bytes, main_decoder=main_decoder)
    except ValueError as error:
        fail(f"device-data extraction failed: {error}")
    board_maps = (
        _board_maps_extractor(platform, host_tools=ROOT / "tools")
        if BOARD_MAPS_GROUP in declarations
        else None
    )

    staging = Path(tempfile.mkdtemp(prefix=".phone-", dir=destination.parent))
    selected = False
    try:
        extracted = _extract_groups(
            target,
            device_data["parser"],
            declarations,
            nand,
            board_maps=board_maps,
        )
        materialize_device_data(staging, extracted, declarations, retain_originals=False)
        staging.replace(destination)
        selected = True
    finally:
        if not selected and staging.exists():
            shutil.rmtree(staging)
    return destination
