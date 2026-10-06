# SPDX-License-Identifier: GPL-2.0-only
"""Plan and remove freshly selected, verified managed-cache directories."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT
from fplinux_cli.manifests.values import TARGET_NAME

from .alpine import apk_entries, rootfs_entries
from .builds import host_tool_entries, linux_cache_entries, workspace_entries
from .logs import log_entries, log_retention_entries
from .model import InventoryEntry, PruneApplyResult, PrunePlan, PruneSafetyError
from .paths import managed_candidate_directory
from .profiles import profile_cache_entries, profile_check_receipt_entries

if TYPE_CHECKING:
    from pathlib import Path


def prune(
    *,
    cache: Path | None = None,
    apply: bool = False,
) -> None:
    """Print a dry run or remove its freshly recomputed candidates."""
    cache_path = cache or ROOT / ".cache"
    result = apply_prune(cache_path) if apply else plan_prune(cache_path)
    print(result.as_text(), end="")


def apply_prune(cache: Path) -> PruneApplyResult:
    """Delete only candidates from a fresh plan while the CLI holds its global lock."""
    plan = plan_prune(cache)
    removed: list[str] = []
    logical = 0
    allocated = 0
    for entry in plan.candidates:
        destination = managed_candidate_directory(cache, entry.path)
        shutil.rmtree(destination)
        removed.append(entry.path)
        logical += entry.logical_bytes or 0
        allocated += entry.allocated_bytes or 0
    return PruneApplyResult(tuple(removed), logical, allocated)


def discard_obsolete_rootfs(cache: Path) -> tuple[str, ...]:
    """Discard only rootfs generations superseded by every current target/profile recipe."""
    removed: list[str] = []
    for entry in rootfs_entries(cache):
        if entry.action != "candidate":
            continue
        destination = managed_candidate_directory(cache, entry.path)
        shutil.rmtree(destination)
        removed.append(entry.path)
    return tuple(removed)


def discard_obsolete_apks(cache: Path) -> tuple[str, ...]:
    """Discard only aport cache slots absent from every current target/profile closure."""
    removed: list[str] = []
    for entry in apk_entries(cache):
        if entry.action != "candidate":
            continue
        destination = managed_candidate_directory(cache, entry.path)
        shutil.rmtree(destination)
        removed.append(entry.path)
    return tuple(removed)


def discard_superseded_profile_logs(
    cache: Path,
    command: str,
    *,
    profile: str,
    target: str | None = None,
) -> tuple[str, ...]:
    """Keep one current profile log group bounded without touching default command logs."""
    if TARGET_NAME.fullmatch(profile) is None:
        raise PruneSafetyError(f"invalid managed profile log name: {profile!r}")
    if command == "check" and target is None:
        root = cache / "logs" / "check" / "profiles" / profile
        identity = f"logs/check/profiles/{profile}"
        label = f"check profiles/{profile}"
    elif command == "build" and isinstance(target, str) and TARGET_NAME.fullmatch(target):
        root = cache / "logs" / "build" / target / "profiles" / profile
        identity = f"logs/build/{target}/profiles/{profile}"
        label = f"build {target}/profiles/{profile}"
    else:
        message = "invalid managed profile log group"
        raise PruneSafetyError(message)
    removed: list[str] = []
    for entry in log_entries(root, label=label, identity=identity):
        if entry.action != "candidate":
            continue
        destination = managed_candidate_directory(cache, entry.path)
        shutil.rmtree(destination)
        removed.append(entry.path)
    return tuple(removed)


def plan_prune(cache: Path) -> PrunePlan:
    """List bounded disposable state across the explicit managed namespaces."""
    entries: list[InventoryEntry] = []
    entries.extend(apk_entries(cache))
    entries.extend(host_tool_entries(cache))
    entries.extend(rootfs_entries(cache))
    entries.extend(profile_cache_entries(cache))
    entries.extend(profile_check_receipt_entries(cache))
    entries.extend(linux_cache_entries(cache))
    entries.extend(log_retention_entries(cache))
    entries.extend(workspace_entries(cache))
    return PrunePlan(tuple(sorted(entries, key=lambda entry: entry.path)))
