# SPDX-License-Identifier: GPL-2.0-only
"""Resolve the three source archives used by the SC9820E RAM loader."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from fplinux_cli.common import fail
from fplinux_cli.manifests.source_catalog import source_build_metadata
from fplinux_cli.manifests.values import (
    basename_value,
    nonempty_string,
    relative_value,
    sha256_value,
)


@dataclass(frozen=True)
class LoaderSource:
    """One pinned source archive shared by build, dependency and source delivery."""

    role: str
    source_lock: str
    url: str
    sha256: str
    cache_name: str
    archive_prefix: str


def loader_sources(build: dict[str, Any], sources: dict[str, Any]) -> tuple[LoaderSource, ...]:
    """Resolve the selected first stage, board initializer and secure monitor archives."""
    result = []
    for role in ("fdl1", "fdl2", "sml"):
        name = basename_value(build[f"{role}_source"], f"loader build {role}_source")
        source = source_build_metadata(name, sources.get(name))
        url = nonempty_string(source.get("archive_url"), f"loader source {name} archive_url")
        if not url.startswith("https://"):
            fail(f"loader source {name} archive_url must use HTTPS")
        prefix = relative_value(
            source.get("archive_prefix"), f"loader source {name} archive_prefix"
        )
        result.append(
            LoaderSource(
                role=role,
                source_lock=name,
                url=url,
                sha256=sha256_value(
                    source.get("archive_sha256"), f"loader source {name} archive_sha256"
                ),
                cache_name=source["destination"].removeprefix("downloads/"),
                archive_prefix=prefix,
            )
        )
    return tuple(result)
