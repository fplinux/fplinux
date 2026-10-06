# SPDX-License-Identifier: GPL-2.0-only
"""Select supported quality scopes and their analyzer caches."""

from __future__ import annotations

from fplinux_cli.common import fail

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
