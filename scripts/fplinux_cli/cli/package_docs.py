# SPDX-License-Identifier: GPL-2.0-only
"""Collect a current offline documentation export for host archives."""

from __future__ import annotations

import json
import tomllib
from typing import TYPE_CHECKING, NoReturn

from fplinux_cli import common
from fplinux_cli.common import fail, payload_digest, read_json_object, sha256_bytes

if TYPE_CHECKING:
    from pathlib import Path


def _export_required(reason: str) -> NoReturn:
    fail(f"offline documentation {reason}; run npm run build:offline in site")


def _regular_files(root: Path, *, excluded: frozenset[str] = frozenset()) -> list[Path]:
    if root.is_symlink() or not root.is_dir():
        _export_required("is missing or invalid")
    files: list[Path] = []
    for source in sorted(root.iterdir()):
        if source.name in excluded:
            continue
        if source.is_symlink():
            _export_required("contains an invalid symlink")
        if source.is_dir():
            files.extend(_regular_files(source, excluded=excluded))
        elif source.is_file():
            files.append(source)
        else:
            _export_required("contains an invalid file")
    return files


def offline_documentation() -> dict[str, bytes]:
    """Require matching source and output receipts, returning only generated docs."""
    site = common.ROOT / "site"
    output = common.ROOT / ".cache/out/site-offline"
    receipt = read_json_object(common.ROOT / ".cache/out/site-offline-receipt.json")
    if receipt is None:
        _export_required("is missing or invalid")
    inputs: dict[str, bytes] = {}
    executables: set[str] = set()
    for source in _regular_files(site, excluded=frozenset({"node_modules", "dist", ".astro"})):
        relative = source.relative_to(common.ROOT).as_posix()
        inputs[relative] = source.read_bytes()
        if source.stat().st_mode & 0o111:
            executables.add(relative)
    try:
        pages = json.loads((site / "src/data/phones.json").read_text(encoding="utf-8"))
        sources: set[Path] = set()
        for page in pages:
            target_directory = common.ROOT / "targets" / page["id"]
            target_path = target_directory / "target.toml"
            target = tomllib.loads(target_path.read_text(encoding="utf-8"))
            sources.update(
                {
                    target_path,
                    common.ROOT / "platforms" / target["platform"] / "platform.toml",
                    target_directory / target["display"]["panel"],
                }
            )
        for source in sorted(sources):
            relative = source.relative_to(common.ROOT).as_posix()
            inputs[relative] = source.read_bytes()
            if source.stat().st_mode & 0o111:
                executables.add(relative)
    except OSError, ValueError, KeyError, TypeError:
        _export_required("has invalid phone sources")
    if receipt.get("inputs_sha256") != payload_digest(inputs, executables):
        _export_required("is stale")
    records = receipt.get("files")
    if not isinstance(records, dict):
        _export_required("has an invalid export receipt")
    files: dict[str, bytes] = {}
    for source in _regular_files(output):
        relative = source.relative_to(output).as_posix()
        data = source.read_bytes()
        record = records.get(relative)
        if (
            not isinstance(record, dict)
            or record.get("sha256") != sha256_bytes(data)
            or record.get("size") != len(data)
            or record.get("mode") != (source.stat().st_mode & 0o777)
        ):
            _export_required("differs from its successful export")
        files[relative] = data
    if set(files) != set(records) or "index.html" not in files:
        _export_required("is incomplete")
    return {f"docs/{relative}": data for relative, data in files.items()}
