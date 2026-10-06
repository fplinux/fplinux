# SPDX-License-Identifier: GPL-2.0-only
"""Resolve and revalidate only explicitly managed cache deletion paths."""

from __future__ import annotations

import re
import stat
from typing import TYPE_CHECKING

from fplinux_cli.manifests.values import TARGET_NAME

from .model import PruneSafetyError

if TYPE_CHECKING:
    from pathlib import Path

WORKSPACE_NAMESPACES = frozenset({"quality-workspaces", "workspaces"})
_MANAGED_NAMESPACES = WORKSPACE_NAMESPACES | {"apks", "host-tools", "rootfs"}
SOURCE_SHA256 = re.compile(r"^[0-9a-f]{64}$")


def _candidate_destination(cache: Path, identity: str) -> Path:  # noqa: PLR0911 -- safe shapes.
    """Return the exact managed cache directory addressed by one candidate identity."""
    parts = identity.split("/")
    if len(parts) == 2 and parts[0] in _MANAGED_NAMESPACES and parts[1]:
        return cache / parts[0] / parts[1]
    if len(parts) == 3 and parts[:2] in (
        ["logs", "check"],
        ["logs", "format"],
        ["logs", "setup"],
    ):
        return cache.joinpath(*parts)
    if (
        len(parts) == 3
        and parts[:2] == ["check-results", "profiles"]
        and TARGET_NAME.fullmatch(parts[2]) is not None
    ):
        return cache.joinpath(*parts)
    if len(parts) == 4 and parts[:2] == ["logs", "build"] and parts[2] and parts[3]:
        return cache.joinpath(*parts)
    if (
        len(parts) == 4
        and parts[0] == "out"
        and parts[2] == "profiles"
        and TARGET_NAME.fullmatch(parts[1]) is not None
        and TARGET_NAME.fullmatch(parts[3]) is not None
    ):
        return cache.joinpath(*parts)
    if (
        len(parts) == 5
        and parts[:2] == ["analysis", "sparse"]
        and TARGET_NAME.fullmatch(parts[2]) is not None
        and parts[3] == "profiles"
        and TARGET_NAME.fullmatch(parts[4]) is not None
    ):
        return cache.joinpath(*parts)
    if (
        len(parts) == 3
        and parts[0] == "linux"
        and parts[1] in {"sources", "originals", "staging"}
        and SOURCE_SHA256.fullmatch(parts[2]) is not None
    ):
        return cache.joinpath(*parts)
    if (
        len(parts) == 4
        and parts[:3] == ["logs", "check", "profiles"]
        and TARGET_NAME.fullmatch(parts[3]) is not None
    ):
        return cache.joinpath(*parts)
    if (
        len(parts) == 5
        and parts[:3] == ["logs", "check", "profiles"]
        and TARGET_NAME.fullmatch(parts[3]) is not None
        and parts[4]
    ):
        return cache.joinpath(*parts)
    if (
        len(parts) == 5
        and parts[:2] == ["logs", "build"]
        and TARGET_NAME.fullmatch(parts[2]) is not None
        and parts[3] == "profiles"
        and TARGET_NAME.fullmatch(parts[4]) is not None
    ):
        return cache.joinpath(*parts)
    if (
        len(parts) == 6
        and parts[:2] == ["logs", "build"]
        and TARGET_NAME.fullmatch(parts[2]) is not None
        and parts[3] == "profiles"
        and TARGET_NAME.fullmatch(parts[4]) is not None
        and parts[5]
    ):
        return cache.joinpath(*parts)
    raise PruneSafetyError(f"invalid managed candidate: {identity}")


def managed_candidate_directory(cache: Path, identity: str) -> Path:
    """Resolve one candidate through real cache directories immediately before removal."""
    destination = _candidate_destination(cache, identity)
    _require_real_directory(cache, "cache root")
    current = cache
    for component in destination.relative_to(cache).parts:
        current /= component
        _require_real_directory(current, f"managed candidate component: {identity}")
    return destination


def _require_real_directory(path: Path, label: str) -> None:
    """Reject a missing, non-directory or symlinked deletion path component."""
    try:
        state = path.lstat()
    except OSError as error:
        raise PruneSafetyError(f"{label} is missing or unreadable: {path}") from error
    if stat.S_ISLNK(state.st_mode) or not stat.S_ISDIR(state.st_mode):
        raise PruneSafetyError(f"{label} is not a real directory: {path}")


def tree_size(root: Path) -> tuple[int, int]:
    logical = 0
    allocated = 0
    for path in (root, *root.rglob("*")):
        try:
            state = path.lstat()
        except OSError:
            continue
        logical += state.st_size
        allocated += state.st_blocks * 512
    return logical, allocated
