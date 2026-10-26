# SPDX-License-Identifier: GPL-2.0-only
"""Admit persistent phone inputs by their acquisition and preparation dependencies."""

from __future__ import annotations

import json
import os
from dataclasses import asdict
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import canonical_json_bytes, fail, sha256_bytes, sha256_file
from fplinux_cli.device_data.inputs import capture_phone_data
from fplinux_cli.runtime.nand_backup import read_backup_record, require_declared_chip

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.device_data.original_files import OriginalFilesSource
    from fplinux_cli.device_data.prepare import DeviceDataSpec, DeviceDataTools
    from fplinux_cli.runtime.nand_backup import BackupRecord


def preparation_identity(spec: DeviceDataSpec, tools: DeviceDataTools) -> str:
    """Bind the selected preparation code to its actual optional native ECC decoder."""
    if spec.preparation_recipe is None:
        fail("This phone-data selection has no preparation implementation")
    native = None
    if spec.nand is not None and "ecc_format" in spec.nand:
        from .prepare import NAND_ECC_TOOL  # noqa: PLC0415 -- selected native decoder.

        if tools.host_tools is None:
            fail("device-data preparation requires its native NAND ECC decoder")
        executable = NAND_ECC_TOOL + (".exe" if os.name == "nt" else "")
        native = sha256_file(tools.host_tools / executable)
    return sha256_bytes(
        canonical_json_bytes({"implementation": spec.preparation_recipe, "native_ecc": native})
    )


def current_backup(backup: Path, *, target: str, nand: dict[str, Any]) -> BackupRecord | None:
    """Treat an absent, incompatible or incomplete acquisition record as a cache miss."""
    if not backup.is_file() or backup.is_symlink():
        return None
    raw = backup.read_bytes()
    try:
        record = read_backup_record(backup, raw)
        if (
            record is None
            or record.target != target
            or record.acquisition_recipe != nand["acquisition_recipe"]
        ):
            return None
        require_declared_chip(
            record.geometry,
            target=target,
            declared_id=nand.get("id"),
            declared_raw_page_bytes=nand.get("raw_page_bytes"),
            source="saved NAND backup",
        )
    except SystemExit:
        return None
    return record


def prepared_data_current(
    spec: DeviceDataSpec,
    tools: DeviceDataTools,
    destination: Path,
    backup: BackupRecord,
) -> bool:
    """Require current implementation, source geometry and independently admitted output bytes."""
    return _prepared_data_matches(
        spec,
        tools,
        destination,
        source={
            "kind": "physical-nand",
            "size": backup.geometry.raw_bytes,
            "sha256": backup.sha256,
        },
        geometry=asdict(backup.geometry),
    )


def current_original_files(spec: DeviceDataSpec, directory: Path) -> OriginalFilesSource | None:
    """Reuse selected files only after their acquisition and complete bytes are admitted."""
    from .emmc import validate_file_selections  # noqa: PLC0415 -- selected eMMC acquisition.
    from .original_files import (  # noqa: PLC0415 -- selected original-file cache.
        read_original_files,
    )

    if spec.emmc is None:
        fail("original-file acquisition requires the selected eMMC reader")
    try:
        source = read_original_files(spec.target, directory / "manifest.json")
        selections = validate_file_selections(spec.emmc["files"])
    except OSError, ValueError, SystemExit:
        return None
    if source.acquisition_recipe != spec.emmc["acquisition_recipe"]:
        return None
    if set(source.files) != {selection.name for selection in selections}:
        return None
    if any(len(source.files[selection.name]) > selection.max_bytes for selection in selections):
        return None
    return source


def prepared_originals_current(
    spec: DeviceDataSpec,
    tools: DeviceDataTools,
    destination: Path,
    source: OriginalFilesSource,
) -> bool:
    """Keep preparation identity separate from acquisition of reusable original files."""
    return _prepared_data_matches(
        spec,
        tools,
        destination,
        source={
            "kind": "original-files",
            "size": len(source.manifest),
            "sha256": sha256_bytes(source.manifest),
        },
        geometry=None,
    )


def _prepared_data_matches(
    spec: DeviceDataSpec,
    tools: DeviceDataTools,
    destination: Path,
    *,
    source: dict[str, Any],
    geometry: dict[str, Any] | None,
) -> bool:
    """Admit the last selected preparation without attributing preserved groups to its source."""
    receipt = destination / "receipt.json"
    try:
        value = json.loads(receipt.read_text(encoding="utf-8"))
        if not isinstance(value, dict) or set(value) != {
            "source",
            "groups",
            "preparation_recipe",
            "geometry",
        }:
            return False
        if (
            value["preparation_recipe"] != preparation_identity(spec, tools)
            or value["geometry"] != geometry
            or value["source"] != source
        ):
            return False
        groups = value["groups"]
        if not isinstance(groups, list):
            return False
        saved: dict[str, Any] = {}
        for group in groups:
            if (
                not isinstance(group, dict)
                or not {"name", "originals", "prepared"}.issubset(group)
                or set(group) - {"name", "originals", "prepared", "reports"}
                or not isinstance(group["name"], str)
                or group["name"] in saved
            ):
                return False
            saved[group["name"]] = group["prepared"]
        actual = capture_phone_data(spec.required_declarations, destination)
    except OSError, ValueError, SystemExit:
        return False
    return saved == {
        name: [firmware.recipe_record() for firmware in files] for name, files in actual.items()
    }
