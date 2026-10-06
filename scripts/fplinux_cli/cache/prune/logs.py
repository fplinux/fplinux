# SPDX-License-Identifier: GPL-2.0-only
"""Bound retention of recognized command and profile log generations."""

from __future__ import annotations

import json
import re
from typing import TYPE_CHECKING

from fplinux_cli.manifests.values import TARGET_NAME

from .model import InventoryEntry
from .paths import tree_size
from .profiles import declared_profiles

if TYPE_CHECKING:
    from pathlib import Path

_LOG_RUN_LIMIT = 10
_RUN_ID = re.compile(r"^\d{8}T\d{6}Z-p\d+(?:-\d+)?$")


def _log_run_matches(path: Path, *, label: str, identity: str) -> bool:
    """Return whether one directory is a current host-created command log."""
    if not path.is_dir() or path.is_symlink() or _RUN_ID.fullmatch(path.name) is None:
        return False
    metadata = path / "run.json"
    if not metadata.is_file() or metadata.is_symlink():
        return False
    try:
        decoded = json.loads(metadata.read_text())
    except OSError, UnicodeDecodeError, json.JSONDecodeError:
        return False
    return (
        isinstance(decoded, dict)
        and decoded.get("display_root") == f".cache/{identity}"
        and decoded.get("label") == label
        and decoded.get("parent") is None
    )


def log_entries(root: Path, *, label: str, identity: str) -> list[InventoryEntry]:
    """Retain the newest bounded set of one CLI command's log runs."""
    if not root.is_dir() or root.is_symlink():
        return []
    runs = [
        path
        for path in root.iterdir()
        if _log_run_matches(path, label=label, identity=f"{identity}/{path.name}")
    ]
    entries: list[InventoryEntry] = []
    for index, path in enumerate(sorted(runs, key=lambda item: item.name, reverse=True)):
        entry_identity = f"{identity}/{path.name}"
        if index < _LOG_RUN_LIMIT:
            entries.append(
                InventoryEntry(
                    entry_identity,
                    "protected",
                    f"within {_LOG_RUN_LIMIT}-run log retention",
                    None,
                    None,
                )
            )
            continue
        logical, allocated = tree_size(path)
        entries.append(
            InventoryEntry(
                entry_identity,
                "candidate",
                f"older than {_LOG_RUN_LIMIT}-run log retention",
                logical,
                allocated,
            )
        )
    return entries


def _profile_log_entries(  # noqa: PLR0913 -- profile log ownership is explicit.
    root: Path,
    *,
    label: str,
    identity: str,
    profile: str,
    declared: dict[str, frozenset[str]] | None,
    target: str | None = None,
) -> list[InventoryEntry]:
    """Keep declared profile logs bounded and discard every valid orphaned run."""
    entries = log_entries(root, label=label, identity=identity)
    if declared is None:
        return entries
    known = (
        profile in declared.get(target, frozenset())
        if target is not None
        else any(profile in profiles for profiles in declared.values())
    )
    if known:
        return entries
    children = tuple(root.iterdir()) if root.is_dir() and not root.is_symlink() else ()
    valid_names = {entry.path.rsplit("/", maxsplit=1)[-1] for entry in entries}
    if not entries or {path.name for path in children} != valid_names:
        return []
    logical, allocated = tree_size(root)
    return [
        InventoryEntry(
            identity,
            "candidate",
            "orphaned managed profile log root",
            logical,
            allocated,
        )
    ]


def log_retention_entries(cache: Path) -> list[InventoryEntry]:
    """Classify only generated check, format, setup, and per-target build logs."""
    logs = cache / "logs"
    entries = [
        *log_entries(logs / "check", label="check", identity="logs/check"),
        *log_entries(logs / "format", label="format", identity="logs/format"),
        *log_entries(logs / "setup", label="setup", identity="logs/setup"),
    ]
    declared = declared_profiles()
    check_profiles = logs / "check" / "profiles"
    if check_profiles.is_dir() and not check_profiles.is_symlink():
        for profile in sorted(check_profiles.iterdir(), key=lambda item: item.name):
            if (
                profile.is_dir()
                and not profile.is_symlink()
                and TARGET_NAME.fullmatch(profile.name)
            ):
                identity = f"logs/check/profiles/{profile.name}"
                entries.extend(
                    _profile_log_entries(
                        profile,
                        label=f"check profiles/{profile.name}",
                        identity=identity,
                        profile=profile.name,
                        declared=declared,
                    )
                )
    builds = logs / "build"
    if not builds.is_dir() or builds.is_symlink():
        return entries
    for target in sorted(builds.iterdir(), key=lambda item: item.name):
        if not target.is_dir() or target.is_symlink():
            continue
        identity = f"logs/build/{target.name}"
        entries.extend(log_entries(target, label=f"build {target.name}", identity=identity))
        profiles = target / "profiles"
        if not profiles.is_dir() or profiles.is_symlink():
            continue
        for profile in sorted(profiles.iterdir(), key=lambda item: item.name):
            if (
                profile.is_dir()
                and not profile.is_symlink()
                and TARGET_NAME.fullmatch(profile.name)
            ):
                profile_identity = f"{identity}/profiles/{profile.name}"
                entries.extend(
                    _profile_log_entries(
                        profile,
                        label=f"build {target.name}/profiles/{profile.name}",
                        identity=profile_identity,
                        profile=profile.name,
                        declared=declared,
                        target=target.name,
                    )
                )
    return entries
