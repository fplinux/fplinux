# SPDX-License-Identifier: GPL-2.0-only
"""Load pinned loader assets and their bundle destinations."""

from __future__ import annotations

import tomllib
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import fail
from fplinux_cli.manifests.values import exact_table, nonempty_string, relative_value, sha256_value

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path


def load_asset_sources(path: Path) -> list[dict[str, Any]]:
    """Read the platform's pinned first-stage RAM loader."""
    if path.is_symlink() or not path.is_file():
        fail(f"asset lock is missing or invalid: {path}")
    try:
        with path.open("rb") as stream:
            document = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f"asset lock is invalid: {error}")
    return resolve_asset_sources(document)


def resolve_asset_sources(common_mapping: object) -> list[dict[str, Any]]:
    """Resolve the shared FDL1 source and its independently checked output."""
    common = exact_table(common_mapping, {"fdl1"}, "asset lock")
    source = exact_table(
        common["fdl1"], {"url", "sha256", "cache_name", "license", "path"}, "asset fdl1"
    )
    url = nonempty_string(source["url"], "asset fdl1 url")
    if not url.startswith("https://"):
        fail("asset fdl1 url must use HTTPS")
    sha256_value(source["sha256"], "asset fdl1 source")
    relative_value(source["cache_name"], "asset fdl1 cache_name")
    nonempty_string(source["license"], "asset fdl1 license")
    relative_value(source["path"], "asset fdl1 path")
    return [
        {
            "id": "fdl1",
            "kind": "file",
            **{field: source[field] for field in ("url", "sha256", "cache_name", "license")},
            "output": [{"role": "fdl1", "path": source["path"], "sha256": source["sha256"]}],
        }
    ]


def asset_bundle_paths(sources: Sequence[dict[str, Any]]) -> dict[str, str]:
    """Derive bundle paths solely from the resolved loader outputs."""
    return {
        str(output["role"]): f"assets/{output['path']}"
        for source in sources
        for output in source["output"]
    }
