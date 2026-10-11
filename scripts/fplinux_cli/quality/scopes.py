# SPDX-License-Identifier: GPL-2.0-only
"""Select quality scopes, common C inputs and required analyzer caches."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.alpine import registration
from fplinux_cli.common import fail

if TYPE_CHECKING:
    from pathlib import PurePath

CHECK_SCOPES = (
    "repository",
    "source",
    "container",
    "metadata",
    "docs",
    "spelling",
    "secrets",
    "licenses",
    "python",
    "shell",
    "alpine",
    "c",
    "kernel",
)


SOURCE_CHECK_SCOPES = CHECK_SCOPES[1:-1]


def is_project_c_source(path: PurePath) -> bool:
    """Recognize C/H shared by formatting discovery and check input selection."""
    return path.suffix in {".c", ".h"} and (
        path.parts[:2] == ("alpine", "aports")
        or path.as_posix() in registration.SHARED_APORT_SOURCE_PATHS
        or path.parts[:1] == ("tests",)
        or (
            len(path.parts) >= 4
            and path.parts[0] in {"platforms", "targets"}
            and path.parts[2] in {"common", "uboot"}
        )
    )


def resolve_check_scopes(scopes: list[str]) -> tuple[str, ...]:
    """Validate, deduplicate and canonicalize a check selection."""
    requested = set(scopes)
    unknown = requested.difference(CHECK_SCOPES)
    if unknown:
        fail(f"unknown check scope: {', '.join(sorted(unknown))}")
    return tuple(scope for scope in CHECK_SCOPES if not scopes or scope in requested)


def analyzer_cache_names(scopes: tuple[str, ...]) -> tuple[str, ...]:
    """Return analyzer caches required by the selected scopes."""
    required: set[str] = set()
    if "kernel" in scopes:
        required.update(("analysis", "downloads", "linux"))
    return tuple(name for name in ("analysis", "downloads", "linux") if name in required)
