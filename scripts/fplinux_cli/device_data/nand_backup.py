# SPDX-License-Identifier: GPL-2.0-only
"""Admit and publish saved raw NAND images and their acquisition receipts."""

from __future__ import annotations

import json
import os
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING, cast

from fplinux_cli.common import (
    canonical_json_bytes,
    fail,
    is_sha256,
    replace_file_atomically,
    sha256_bytes,
    sha256_file,
)
from fplinux_cli.device_data.nand_geometry import (
    GEOMETRY_SIZE_KEYS,
    NandGeometry,
    require_consistent_geometry,
    require_declared_chip,
)

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import Any, BinaryIO

_RECEIPT_KEYS = frozenset(
    {"id_bytes", "chip", *GEOMETRY_SIZE_KEYS, "sha256", "target", "acquisition_recipe"}
)


@dataclass(frozen=True)
class BackupRecord:
    """Validated geometry and acquisition identity bound to exact backup bytes."""

    geometry: NandGeometry
    target: str
    acquisition_recipe: str
    sha256: str


def backup_receipt_path(backup: Path) -> Path:
    """Return the geometry receipt path that belongs beside one backup file."""
    return backup.with_name(f"{backup.name}.json")


def read_backup_bytes(path: Path) -> bytes:
    """Read one regular NAND backup without changing or replacing it."""
    if path.is_symlink() or not path.is_file():
        fail(f"NAND backup is missing or invalid: {path}")
    try:
        return path.read_bytes()
    except OSError as error:
        fail(f"NAND backup cannot be read: {path}: {error}")


def read_backup_record(backup: Path, raw: bytes) -> BackupRecord | None:
    """Admit the acquisition receipt recorded for exactly these backup bytes."""
    receipt = backup_receipt_path(backup)
    if not receipt.exists() and not receipt.is_symlink():
        return None
    if receipt.is_symlink() or not receipt.is_file():
        fail(f"NAND backup receipt is not a regular file: {receipt}")
    try:
        value = json.loads(receipt.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"NAND backup receipt cannot be read: {receipt}: {error}")
    if not isinstance(value, dict) or set(value) != _RECEIPT_KEYS:
        fail(f"NAND backup receipt must contain exactly: {', '.join(sorted(_RECEIPT_KEYS))}")
    for key in ("id_bytes", "chip", "sha256", "target", "acquisition_recipe"):
        if not isinstance(value[key], str):
            fail(f"NAND backup receipt {key} must be a string: {receipt}")
    for key in GEOMETRY_SIZE_KEYS:
        if type(value[key]) is not int:
            fail(f"NAND backup receipt {key} must be an integer: {receipt}")
    for key in ("sha256", "acquisition_recipe"):
        if not is_sha256(value[key]):
            fail(f"NAND backup receipt {key} must be a lowercase SHA-256 digest: {receipt}")
    geometry = require_consistent_geometry(
        NandGeometry(
            id_bytes=value["id_bytes"],
            chip=value["chip"],
            page_main_bytes=value["page_main_bytes"],
            oob_bytes=value["oob_bytes"],
            pages_per_block=value["pages_per_block"],
            block_count=value["block_count"],
            raw_bytes=value["raw_bytes"],
        ),
        f"NAND backup receipt {receipt}",
    )
    if geometry.raw_bytes != len(raw):
        fail(
            f"NAND backup receipt {receipt} describes a {geometry.raw_bytes}-byte backup; "
            f"{backup} has {len(raw)} bytes"
        )
    if value["sha256"] != sha256_bytes(raw):
        fail(f"NAND backup receipt {receipt} describes different backup bytes than {backup}")
    return BackupRecord(geometry, value["target"], value["acquisition_recipe"], value["sha256"])


def read_backup_geometry(backup: Path, raw: bytes) -> NandGeometry | None:
    """Return the device-reported geometry recorded for exactly these backup bytes."""
    record = read_backup_record(backup, raw)
    return None if record is None else record.geometry


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


def require_backup_destination(output: Path, acquisition_recipe: str) -> None:
    """Admit the raw destination and receipt before reading or importing an image."""
    if not is_sha256(acquisition_recipe):
        fail("NAND acquisition recipe must be a lowercase SHA-256 digest")
    if output.name in {"", ".", ".."}:
        fail("NAND backup output must name a file")
    parent = output.parent
    if parent.is_symlink() or not parent.is_dir():
        fail(f"NAND backup output directory is missing or invalid: {parent}")
    if output.is_symlink() or (output.exists() and not output.is_file()):
        fail(f"NAND backup output is not a regular file: {output}")
    receipt = backup_receipt_path(output)
    if receipt.is_symlink() or (receipt.exists() and not receipt.is_file()):
        fail(f"NAND backup receipt is not a regular file: {receipt}")


def _publish_backup(
    temporary: Path,
    output: Path,
    geometry: NandGeometry,
    *,
    target: str,
    acquisition_recipe: str,
) -> str:
    """Synchronize one complete image and bind its receipt only after replacement."""
    actual_size = temporary.stat().st_size
    if actual_size != geometry.raw_bytes:
        fail(f"incomplete raw NAND image: expected {geometry.raw_bytes} bytes, got {actual_size}")
    digest = sha256_file(temporary)
    record = {
        **asdict(geometry),
        "sha256": digest,
        "target": target,
        "acquisition_recipe": acquisition_recipe,
    }
    receipt = backup_receipt_path(output)
    # Windows cannot open a directory through the POSIX descriptor API.
    directory_descriptor = (
        None if os.name == "nt" else os.open(output.parent, os.O_RDONLY | os.O_DIRECTORY)
    )
    try:
        # A receipt must never describe bytes from the image being replaced.
        receipt.unlink(missing_ok=True)
        if directory_descriptor is not None:
            os.fsync(directory_descriptor)
        temporary.replace(output)
        if directory_descriptor is not None:
            os.fsync(directory_descriptor)
        replace_file_atomically(receipt, canonical_json_bytes(record), 0o600)
        if directory_descriptor is not None:
            os.fsync(directory_descriptor)
    finally:
        if directory_descriptor is not None:
            os.close(directory_descriptor)
    return digest


def write_backup(
    output: Path,
    geometry: NandGeometry,
    *,
    target: str,
    acquisition_recipe: str,
    write_image: Callable[[BinaryIO], object],
) -> str:
    """Stream into an admitted destination's temporary file, then publish and return its digest."""
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=output.parent, prefix=f".{output.name}.", mode="w+b", delete=False
        ) as stream:
            temporary = Path(stream.name)
            temporary.chmod(0o600)
            write_image(cast("BinaryIO", stream))
            stream.flush()
            os.fsync(stream.fileno())
        return _publish_backup(
            temporary, output, geometry, target=target, acquisition_recipe=acquisition_recipe
        )
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def save_backup_bytes(
    output: Path,
    raw: bytes,
    geometry: NandGeometry,
    *,
    target: str,
    acquisition_recipe: str,
) -> Path:
    """Publish explicitly admitted backup bytes under the selected physical contract."""
    require_backup_destination(output, acquisition_recipe)
    require_consistent_geometry(geometry, "imported NAND geometry")
    write_backup(
        output,
        geometry,
        target=target,
        acquisition_recipe=acquisition_recipe,
        write_image=lambda stream: stream.write(raw),
    )
    return output


def save_backup_without_receipt(output: Path, raw: bytes) -> Path:
    """Keep explicitly admitted raw bytes without assigning an acquisition identity."""
    replace_file_atomically(output, raw, 0o600)
    backup_receipt_path(output).unlink(missing_ok=True)
    return output
