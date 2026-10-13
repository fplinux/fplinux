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


def load_asset_sources(path: Path, *, loader_maps: object | None) -> list[dict[str, Any]]:
    """Read the platform declaration and resolve the target's optional map pair."""
    if path.is_symlink() or not path.is_file():
        fail(f"asset lock is missing or invalid: {path}")
    try:
        with path.open("rb") as stream:
            document = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f"asset lock is invalid: {error}")
    return resolve_asset_sources(document, loader_maps=loader_maps)


def _loader_source(value: object, *, path_fields: tuple[str, ...], name: str) -> dict[str, Any]:
    """Validate the download pin and destination fields shared by both loader resources."""
    source = exact_table(value, {"url", "sha256", "cache_name", "license", *path_fields}, name)
    url = nonempty_string(source["url"], f"{name} url")
    if not url.startswith("https://"):
        fail(f"{name} url must use HTTPS")
    sha256_value(source["sha256"], f"{name} source")
    relative_value(source["cache_name"], f"{name} cache_name")
    nonempty_string(source["license"], f"{name} license")
    for field in path_fields:
        relative_value(source[field], f"{name} {field}")
    return source


def resolve_asset_sources(
    common_mapping: object, *, loader_maps: object | None
) -> list[dict[str, Any]]:
    """Resolve the common FDL1 and the target's complete, independently pinned map pair."""
    common = exact_table(common_mapping, {"fdl1", "maps"}, "asset lock")
    fdl1 = _loader_source(common["fdl1"], path_fields=("path",), name="asset fdl1")
    maps = _loader_source(
        common["maps"], path_fields=("pinmap_path", "keymap_path"), name="asset maps"
    )
    sources: list[dict[str, Any]] = [
        {
            "id": "fdl1",
            "kind": "file",
            **{field: fdl1[field] for field in ("url", "sha256", "cache_name", "license")},
            "output": [{"role": "fdl1", "path": fdl1["path"], "sha256": fdl1["sha256"]}],
        }
    ]
    if loader_maps is not None:
        selected_maps = exact_table(loader_maps, {"pinmap", "keymap"}, "target loader_maps")
        outputs: list[dict[str, Any]] = []
        for role in ("pinmap", "keymap"):
            name = f"target loader_maps {role}"
            member = exact_table(selected_maps[role], {"member", "sha256"}, name)
            outputs.append(
                {
                    "role": role,
                    "path": maps[f"{role}_path"],
                    "member": relative_value(member["member"], f"{name} member"),
                    "sha256": sha256_value(member["sha256"], f"{name} output"),
                }
            )
        sources.append(
            {
                "id": "maps",
                "kind": "7z",
                **{field: maps[field] for field in ("url", "sha256", "cache_name", "license")},
                "output": outputs,
            }
        )
    paths: set[str] = set()
    for source in sources:
        for output in source["output"]:
            relative = output["path"]
            if relative in paths:
                fail(f"asset output path is duplicated: {relative}")
            paths.add(relative)
    return sources


def asset_bundle_paths(sources: Sequence[dict[str, Any]]) -> dict[str, str]:
    """Derive bundle paths solely from the resolved loader outputs."""
    return {
        str(output["role"]): f"assets/{output['path']}"
        for source in sources
        for output in source["output"]
    }
