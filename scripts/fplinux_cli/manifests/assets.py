# SPDX-License-Identifier: GPL-2.0-only
"""Load pinned loader assets and their bundle destinations."""

from __future__ import annotations

import tomllib
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.manifests.source_catalog import source_build_metadata
from fplinux_cli.manifests.values import exact_table, nonempty_string, relative_value

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence
    from pathlib import Path


def load_asset_sources(path: Path, *, source_root: Path | None = None) -> list[dict[str, Any]]:
    """Read the platform's pinned first-stage RAM loader."""
    if path.is_symlink() or not path.is_file():
        fail(f"asset lock is missing or invalid: {path}")
    try:
        with path.open("rb") as stream:
            document = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f"asset lock is invalid: {error}")
    return resolve_asset_sources(document, source_root=source_root)


def resolve_asset_sources(
    common_mapping: object,
    *,
    source_root: Path | None = None,
    source_records: Mapping[str, Any] | None = None,
) -> list[dict[str, Any]]:
    """Resolve the shared FDL1 source and its independently checked output."""
    lock = exact_table(common_mapping, {"fdl1"}, "asset lock")
    use = exact_table(lock["fdl1"], {"source", "path"}, "asset fdl1")
    if source_root is None:
        source_root = common.ROOT
    if source_records is None:
        source_records = common.load_toml(source_root / "sources.lock.toml")
    source_id = nonempty_string(use["source"], "asset source")
    metadata = source_build_metadata(source_id, source_records.get(source_id))
    if metadata["algorithm"] != "sha256":
        fail("asset fdl1 requires a SHA-256 source pin")
    license_name = nonempty_string(metadata.get("license"), "asset fdl1 license")
    relative_value(use["path"], "asset fdl1 path")
    return [
        {
            "id": "fdl1",
            "kind": "file",
            "source_id": metadata["source_id"],
            "url": metadata["url"],
            "sha256": metadata["checksum"],
            "cache_name": metadata["destination"].removeprefix("downloads/"),
            "license": license_name,
            "output": [{"role": "fdl1", "path": use["path"], "sha256": metadata["checksum"]}],
        }
    ]


def asset_bundle_paths(sources: Sequence[dict[str, Any]]) -> dict[str, str]:
    """Derive bundle paths solely from the resolved loader outputs."""
    return {
        str(output["role"]): f"assets/{output['path']}"
        for source in sources
        for output in source["output"]
    }
