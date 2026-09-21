# SPDX-License-Identifier: GPL-2.0-only
"""Load pinned loader assets and their bundle destinations."""

from __future__ import annotations

import tomllib
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import fail
from fplinux_cli.manifests.values import exact_table, nonempty_string, relative_value, sha256_value

if TYPE_CHECKING:
    from pathlib import Path


def load_asset_lock(path: Path) -> list[dict[str, Any]]:
    """Load the current pinned asset outputs used to construct a RAM bundle."""
    if path.is_symlink() or not path.is_file():
        fail(f"asset lock is missing or invalid: {path}")
    try:
        with path.open("rb") as stream:
            document = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f"asset lock is invalid: {error}")
    root = exact_table(document, {"source"}, "asset lock")
    sources = root.get("source")
    if not isinstance(sources, list) or not sources:
        fail("asset lock source must be a non-empty array")

    source_ids: set[str] = set()
    roles: set[str] = set()
    paths: set[str] = set()
    result: list[dict[str, Any]] = []
    for index, raw_source in enumerate(sources):
        name = f"asset source[{index}]"
        source = exact_table(
            raw_source,
            {"id", "kind", "url", "sha256", "cache_name", "license", "output"},
            name,
        )
        source_id = nonempty_string(source.get("id"), f"{name} id")
        if source_id in source_ids:
            fail(f"{name} id must be unique")
        source_ids.add(source_id)
        kind = source.get("kind")
        if kind not in {"file", "7z"}:
            fail(f"{name} kind must be file or 7z")
        url = nonempty_string(source.get("url"), f"{name} url")
        if not url.startswith("https://"):
            fail(f"{name} url must use HTTPS")
        sha256_value(source.get("sha256"), f"{name} source")
        relative_value(source.get("cache_name"), f"{name} cache_name")
        nonempty_string(source.get("license"), f"{name} license")
        outputs = source.get("output")
        if not isinstance(outputs, list) or not outputs:
            fail(f"{name} output must be a non-empty array")
        normalized_outputs: list[dict[str, Any]] = []
        for output_index, raw_output in enumerate(outputs):
            output_name = f"{name} output[{output_index}]"
            keys = (
                {"role", "path", "sha256", "member"}
                if kind == "7z"
                else {
                    "role",
                    "path",
                    "sha256",
                }
            )
            output = exact_table(raw_output, keys, output_name)
            role = nonempty_string(output.get("role"), f"{output_name} role")
            if role in roles:
                fail(f"{output_name} role must be unique")
            roles.add(role)
            relative = relative_value(output.get("path"), f"{output_name} path")
            if relative in paths:
                fail(f"asset output path is duplicated: {relative}")
            paths.add(relative)
            sha256_value(output.get("sha256"), f"{output_name} output")
            if kind == "7z":
                relative_value(output.get("member"), f"{output_name} member")
            normalized_outputs.append(output)
        result.append({**source, "output": normalized_outputs})
    return result


def asset_bundle_paths(path: Path) -> dict[str, str]:
    """Derive bundle paths solely from the selected asset-lock outputs."""
    return {
        str(output["role"]): f"assets/{output['path']}"
        for source in load_asset_lock(path)
        for output in source["output"]
    }
