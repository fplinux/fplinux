# SPDX-License-Identifier: GPL-2.0-only
"""List and remove bounded disposable FPLinux cache state."""

from __future__ import annotations

import json
import re
import shutil
import stat
from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal

from . import alpine_state
from .common import ROOT
from .config import (
    TARGET_NAME,
    container_image_recipe_digest,
    discover_profiles,
    discover_targets,
    load_platform,
    load_target,
)

if TYPE_CHECKING:
    from pathlib import Path

_WORKSPACE_NAMESPACES = frozenset({"quality-workspaces", "workspaces"})
_MANAGED_NAMESPACES = _WORKSPACE_NAMESPACES | {"apks", "rootfs"}
_LOG_RUN_LIMIT = 10
_RUN_ID = re.compile(r"^\d{8}T\d{6}Z-p\d+(?:-\d+)?$")


@dataclass(frozen=True)
class InventoryEntry:
    """One workspace retention decision."""

    path: str
    action: Literal["candidate", "protected"]
    reason: str
    logical_bytes: int | None
    allocated_bytes: int | None


@dataclass(frozen=True)
class PrunePlan:
    """A read-only workspace prune plan."""

    entries: tuple[InventoryEntry, ...]
    unsafe: tuple[str, ...] = ()

    @property
    def candidates(self) -> tuple[InventoryEntry, ...]:
        """Return stale workspaces eligible for deletion."""
        return tuple(entry for entry in self.entries if entry.action == "candidate")

    @property
    def candidate_logical_bytes(self) -> int:
        """Return the logical size of all candidates."""
        return sum(entry.logical_bytes or 0 for entry in self.candidates)

    @property
    def candidate_allocated_bytes(self) -> int:
        """Return the allocated size of all candidates."""
        return sum(entry.allocated_bytes or 0 for entry in self.candidates)

    def as_json(self) -> str:
        """Render a stable JSON dry-run report."""
        return (
            json.dumps(
                {
                    "candidate_allocated_bytes": self.candidate_allocated_bytes,
                    "candidate_count": len(self.candidates),
                    "candidate_logical_bytes": self.candidate_logical_bytes,
                    "entries": [entry.__dict__ for entry in self.entries],
                    "mode": "dry-run",
                    "unsafe": list(self.unsafe),
                },
                indent=2,
                sort_keys=True,
            )
            + "\n"
        )

    def as_text(self) -> str:
        """Render a human-readable dry-run report."""
        lines = ["prune: dry-run; no cache changes will be made"]
        for entry in self.entries:
            size = _format_sizes(entry.logical_bytes, entry.allocated_bytes)
            lines.append(f"{entry.action}: {entry.path} ({size}): {entry.reason}")
        lines.append(
            "summary: "
            f"{len(self.candidates)} candidates; "
            f"logical={self.candidate_logical_bytes} B; "
            f"allocated={self.candidate_allocated_bytes} B"
        )
        return "\n".join(lines) + "\n"


@dataclass(frozen=True)
class PruneApplyResult:
    """Result of deleting one freshly computed candidate set."""

    removed: tuple[str, ...]
    reclaimed_logical_bytes: int
    reclaimed_allocated_bytes: int
    unsafe: tuple[str, ...] = ()

    def as_json(self) -> str:
        """Render a stable JSON apply report."""
        return (
            json.dumps(
                {
                    "mode": "apply",
                    "reclaimed_allocated_bytes": self.reclaimed_allocated_bytes,
                    "reclaimed_logical_bytes": self.reclaimed_logical_bytes,
                    "removed": list(self.removed),
                    "removed_count": len(self.removed),
                    "unsafe": list(self.unsafe),
                },
                indent=2,
                sort_keys=True,
            )
            + "\n"
        )

    def as_text(self) -> str:
        """Render a human-readable apply report."""
        lines = ["prune: apply"]
        lines.extend(f"removed: {path}" for path in self.removed)
        lines.append(
            "summary: "
            f"{len(self.removed)} removed; "
            f"logical={self.reclaimed_logical_bytes} B; "
            f"allocated={self.reclaimed_allocated_bytes} B"
        )
        return "\n".join(lines) + "\n"


class PruneSafetyError(RuntimeError):
    """Raised when an apply request escapes the managed namespaces."""


def prune(
    *,
    cache: Path | None = None,
    json_output: bool = False,
    apply: bool = False,
) -> None:
    """Print a dry run or remove its freshly recomputed candidates."""
    cache_path = cache or ROOT / ".cache"
    result = apply_prune(cache_path) if apply else plan_prune(cache_path)
    print(result.as_json() if json_output else result.as_text(), end="")


def _current_rootfs_recipes(cache: Path) -> frozenset[str] | None:
    """Return every currently valid rootfs recipe, or ``None`` if that is unknown."""
    try:
        signing_key = alpine_state.signing_key_identity(cache)
        image_recipe = container_image_recipe_digest()
        recipes = {
            alpine_state.alpine_rootfs_recipe(
                image_recipe,
                signing_key,
                alpine_state.selected_packages(
                    load_platform(target_config["platform"]), target_config
                ),
            )
            for target in discover_targets()
            for profile in (None, *discover_profiles(target))
            for target_config in (
                load_target(target) if profile is None else load_target(target, profile),
            )
        }
    except (OSError, ValueError, SystemExit):
        return None
    return frozenset(recipes)


def _rootfs_entries(cache: Path) -> list[InventoryEntry]:
    """Classify immutable Alpine rootfs generations by all current target recipes."""
    rootfs = cache / "rootfs"
    if rootfs.is_symlink():
        return [
            InventoryEntry(
                "rootfs",
                "protected",
                "rootfs cache root is a symlink",
                None,
                None,
            )
        ]
    if not rootfs.is_dir():
        return []
    current = _current_rootfs_recipes(cache)
    entries: list[InventoryEntry] = []
    for path in sorted(rootfs.iterdir(), key=lambda item: item.name):
        identity = f"rootfs/{path.name}"
        if not path.is_dir() or path.is_symlink():
            entries.append(
                InventoryEntry(identity, "protected", "not a rootfs directory", None, None)
            )
        elif current is None:
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "current rootfs recipes are unavailable",
                    None,
                    None,
                )
            )
        elif path.name in current:
            entries.append(
                InventoryEntry(identity, "protected", "current Alpine rootfs", None, None)
            )
        else:
            logical, allocated = _tree_size(path)
            entries.append(
                InventoryEntry(
                    identity,
                    "candidate",
                    "superseded Alpine rootfs",
                    logical,
                    allocated,
                )
            )
    return entries


def _current_apk_packages() -> frozenset[str] | None:
    """Return every current aport name, or ``None`` when declarations are unavailable."""
    try:
        packages: set[str] = set()
        for target in discover_targets():
            for profile in (None, *discover_profiles(target)):
                target_config = (
                    load_target(target) if profile is None else load_target(target, profile)
                )
                platform_config = load_platform(target_config["platform"])
                rootfs_packages = alpine_state.selected_packages(
                    platform_config,
                    target_config,
                )
                packages.update(rootfs_packages)
                packages.update(
                    alpine_state.bundle_packages(
                        platform_config,
                        target_config,
                        rootfs_packages,
                    )
                )
    except (KeyError, OSError, TypeError, ValueError, SystemExit):
        return None
    return frozenset(packages)


def _apk_entries(cache: Path) -> list[InventoryEntry]:
    """Classify one fixed cache slot for every currently declared aport."""
    apks = cache / alpine_state.PACKAGE_CACHE_DIRECTORY
    if apks.is_symlink():
        return [
            InventoryEntry(
                alpine_state.PACKAGE_CACHE_DIRECTORY,
                "protected",
                "APK cache root is a symlink",
                None,
                None,
            )
        ]
    if not apks.is_dir():
        return []
    current = _current_apk_packages()
    entries: list[InventoryEntry] = []
    for path in sorted(apks.iterdir(), key=lambda item: item.name):
        identity = f"{alpine_state.PACKAGE_CACHE_DIRECTORY}/{path.name}"
        if (
            path.is_symlink()
            or not path.is_dir()
            or alpine_state.PACKAGE_ID.fullmatch(path.name) is None
        ):
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "not a managed APK cache directory",
                    None,
                    None,
                )
            )
        elif current is None:
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "current Alpine package closure is unavailable",
                    None,
                    None,
                )
            )
        elif path.name in current:
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "current Alpine package cache",
                    None,
                    None,
                )
            )
        else:
            logical, allocated = _tree_size(path)
            entries.append(
                InventoryEntry(
                    identity,
                    "candidate",
                    "superseded Alpine package cache",
                    logical,
                    allocated,
                )
            )
    return entries


def _declared_profiles() -> dict[str, frozenset[str]] | None:
    """Return all target-owned profile names, or preserve cache if discovery fails."""
    try:
        return {target: frozenset(discover_profiles(target)) for target in discover_targets()}
    except (OSError, ValueError, SystemExit):
        return None


def _profile_slot_entries(
    root: Path,
    identity_root: str,
    declared: dict[str, frozenset[str]] | None,
    *,
    direct_profile: bool = False,
    source_projection: bool = False,
) -> list[InventoryEntry]:
    """Classify managed target/profile slots without touching default target state."""
    if not root.is_dir() or root.is_symlink():
        return []
    entries: list[InventoryEntry] = []
    for target in sorted(root.iterdir(), key=lambda item: item.name):
        if (
            target.is_symlink()
            or not target.is_dir()
            or TARGET_NAME.fullmatch(target.name) is None
        ):
            continue
        profiles = target if direct_profile else target / "profiles"
        if not profiles.is_dir() or profiles.is_symlink():
            continue
        for path in sorted(profiles.iterdir(), key=lambda item: item.name):
            identity = (
                f"{identity_root}/{target.name}/{path.name}"
                if direct_profile
                else f"{identity_root}/{target.name}/profiles/{path.name}"
            )
            if path.is_symlink() or not path.is_dir() or TARGET_NAME.fullmatch(path.name) is None:
                entries.append(
                    InventoryEntry(
                        identity,
                        "protected",
                        "not a managed profile cache directory",
                        None,
                        None,
                    )
                )
            elif declared is None:
                entries.append(
                    InventoryEntry(
                        identity,
                        "protected",
                        "profile declarations are unavailable",
                        None,
                        None,
                    )
                )
            elif path.name in declared.get(target.name, frozenset()):
                separate = (
                    _profile_uses_separate_linux_source(target.name, path.name)
                    if source_projection
                    else True
                )
                if separate is None:
                    entries.append(
                        InventoryEntry(
                            identity,
                            "protected",
                            "profile source integration is unavailable",
                            None,
                            None,
                        )
                    )
                elif separate:
                    entries.append(
                        InventoryEntry(
                            identity,
                            "protected",
                            "declared profile cache",
                            None,
                            None,
                        )
                    )
                else:
                    logical, allocated = _tree_size(path)
                    entries.append(
                        InventoryEntry(
                            identity,
                            "candidate",
                            "profile now reuses the default Linux source",
                            logical,
                            allocated,
                        )
                    )
            else:
                logical, allocated = _tree_size(path)
                entries.append(
                    InventoryEntry(
                        identity,
                        "candidate",
                        "orphaned managed profile cache",
                        logical,
                        allocated,
                    )
                )
    return entries


def _profile_uses_separate_linux_source(target: str, profile: str) -> bool | None:
    """Return whether a profile still needs a dedicated prepared Linux tree."""
    try:
        default = load_target(target)
        selected = load_target(target, profile)
        return any(
            default["linux"][field] != selected["linux"][field]
            for field in ("patches", "copies", "appends", "root")
        )
    except (OSError, ValueError, SystemExit, KeyError, TypeError):
        return None


def _profile_cache_entries(cache: Path) -> list[InventoryEntry]:
    """Remove only orphaned profile-only cache slots in explicit CLI namespaces."""
    declared = _declared_profiles()
    return [
        *_profile_slot_entries(cache / "out", "out", declared),
        *(
            _profile_slot_entries(
                cache / "linux" / "profiles",
                "linux/profiles",
                declared,
                direct_profile=True,
                source_projection=True,
            )
        ),
        *_profile_slot_entries(cache / "analysis" / "sparse", "analysis/sparse", declared),
    ]


def _profile_check_receipt_entries(cache: Path) -> list[InventoryEntry]:
    """Classify fixed profile check receipts removed from every target declaration."""
    root = cache / "check-results" / "profiles"
    if not root.is_dir() or root.is_symlink():
        return []
    declared = _declared_profiles()
    entries: list[InventoryEntry] = []
    for profile in sorted(root.iterdir(), key=lambda item: item.name):
        identity = f"check-results/profiles/{profile.name}"
        if (
            profile.is_symlink()
            or not profile.is_dir()
            or TARGET_NAME.fullmatch(profile.name) is None
        ):
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "not a managed profile receipt directory",
                    None,
                    None,
                )
            )
        elif declared is None:
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "profile declarations are unavailable",
                    None,
                    None,
                )
            )
        elif any(profile.name in profiles for profiles in declared.values()):
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "declared profile check receipt",
                    None,
                    None,
                )
            )
        else:
            logical, allocated = _tree_size(profile)
            entries.append(
                InventoryEntry(
                    identity,
                    "candidate",
                    "orphaned managed profile check receipt",
                    logical,
                    allocated,
                )
            )
    return entries


def _linux_staging_entries(cache: Path) -> list[InventoryEntry]:
    """Classify the fixed, always-disposable prepared-Linux extraction slots."""
    root = cache / "linux" / "staging"
    if not root.is_dir() or root.is_symlink():
        return []
    entries: list[InventoryEntry] = []
    for target in sorted(root.iterdir(), key=lambda item: item.name):
        if (
            target.is_symlink()
            or not target.is_dir()
            or TARGET_NAME.fullmatch(target.name) is None
        ):
            continue
        default = target / "default"
        if default.is_dir() and not default.is_symlink():
            logical, allocated = _tree_size(default)
            entries.append(
                InventoryEntry(
                    f"linux/staging/{target.name}/default",
                    "candidate",
                    "disposable prepared Linux staging slot",
                    logical,
                    allocated,
                )
            )
        profiles = target / "profiles"
        if not profiles.is_dir() or profiles.is_symlink():
            continue
        for profile in sorted(profiles.iterdir(), key=lambda item: item.name):
            if (
                profile.is_dir()
                and not profile.is_symlink()
                and TARGET_NAME.fullmatch(profile.name)
            ):
                logical, allocated = _tree_size(profile)
                entries.append(
                    InventoryEntry(
                        f"linux/staging/{target.name}/profiles/{profile.name}",
                        "candidate",
                        "disposable prepared Linux staging slot",
                        logical,
                        allocated,
                    )
                )
    return entries


def _log_run_matches(path: Path, *, label: str, identity: str) -> bool:
    """Return whether one directory is a current host-created command log."""
    if not path.is_dir() or path.is_symlink() or _RUN_ID.fullmatch(path.name) is None:
        return False
    metadata = path / "run.json"
    if not metadata.is_file() or metadata.is_symlink():
        return False
    try:
        decoded = json.loads(metadata.read_text())
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return False
    return (
        isinstance(decoded, dict)
        and decoded.get("display_root") == f".cache/{identity}"
        and decoded.get("label") == label
        and decoded.get("parent") is None
    )


def _log_entries(root: Path, *, label: str, identity: str) -> list[InventoryEntry]:
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
        logical, allocated = _tree_size(path)
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
    entries = _log_entries(root, label=label, identity=identity)
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
    logical, allocated = _tree_size(root)
    return [
        InventoryEntry(
            identity,
            "candidate",
            "orphaned managed profile log root",
            logical,
            allocated,
        )
    ]


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
    for entry in _log_entries(root, label=label, identity=identity):
        if entry.action != "candidate":
            continue
        destination = _managed_candidate_directory(cache, entry.path)
        shutil.rmtree(destination)
        removed.append(entry.path)
    return tuple(removed)


def _log_retention_entries(cache: Path) -> list[InventoryEntry]:
    """Classify only the generated check, setup, and per-target build logs."""
    logs = cache / "logs"
    entries = [
        *_log_entries(logs / "check", label="check", identity="logs/check"),
        *_log_entries(logs / "setup", label="setup", identity="logs/setup"),
    ]
    declared = _declared_profiles()
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
        entries.extend(_log_entries(target, label=f"build {target.name}", identity=identity))
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


def plan_prune(cache: Path) -> PrunePlan:
    """List disposable snapshots and bounded CLI logs in managed cache namespaces."""
    entries: list[InventoryEntry] = []
    entries.extend(_apk_entries(cache))
    entries.extend(_rootfs_entries(cache))
    entries.extend(_profile_cache_entries(cache))
    entries.extend(_profile_check_receipt_entries(cache))
    entries.extend(_linux_staging_entries(cache))
    entries.extend(_log_retention_entries(cache))
    for namespace in sorted(_WORKSPACE_NAMESPACES):
        root = cache / namespace
        if not root.is_dir() or root.is_symlink():
            continue
        for path in sorted(root.iterdir(), key=lambda item: item.name):
            if not path.is_dir() or path.is_symlink():
                entries.append(
                    InventoryEntry(
                        f"{namespace}/{path.name}",
                        "protected",
                        "not a disposable workspace directory",
                        None,
                        None,
                    )
                )
                continue
            logical, allocated = _tree_size(path)
            entries.append(
                InventoryEntry(
                    f"{namespace}/{path.name}",
                    "candidate",
                    "disposable staged workspace",
                    logical,
                    allocated,
                )
            )
    return PrunePlan(tuple(sorted(entries, key=lambda entry: entry.path)))


def _candidate_destination(cache: Path, identity: str) -> Path:  # noqa: PLR0911 -- safe shapes.
    """Return the exact managed cache directory addressed by one candidate identity."""
    parts = identity.split("/")
    if len(parts) == 2 and parts[0] in _MANAGED_NAMESPACES and parts[1]:
        return cache / parts[0] / parts[1]
    if len(parts) == 3 and parts[:2] in (["logs", "check"], ["logs", "setup"]):
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
        len(parts) == 4
        and parts[:2] == ["linux", "profiles"]
        and TARGET_NAME.fullmatch(parts[2]) is not None
        and TARGET_NAME.fullmatch(parts[3]) is not None
    ) or (
        len(parts) == 5
        and parts[:2] == ["analysis", "sparse"]
        and TARGET_NAME.fullmatch(parts[2]) is not None
        and parts[3] == "profiles"
        and TARGET_NAME.fullmatch(parts[4]) is not None
    ):
        return cache.joinpath(*parts)
    if (
        len(parts) == 4
        and parts[:2] == ["linux", "staging"]
        and TARGET_NAME.fullmatch(parts[2]) is not None
        and parts[3] == "default"
    ):
        return cache.joinpath(*parts)
    if (
        len(parts) == 5
        and parts[:2] == ["linux", "staging"]
        and TARGET_NAME.fullmatch(parts[2]) is not None
        and parts[3] == "profiles"
        and TARGET_NAME.fullmatch(parts[4]) is not None
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


def apply_prune(cache: Path) -> PruneApplyResult:
    """Delete only candidates from a fresh plan while the CLI holds its global lock."""
    plan = plan_prune(cache)
    removed: list[str] = []
    logical = 0
    allocated = 0
    for entry in plan.candidates:
        destination = _managed_candidate_directory(cache, entry.path)
        shutil.rmtree(destination)
        removed.append(entry.path)
        logical += entry.logical_bytes or 0
        allocated += entry.allocated_bytes or 0
    return PruneApplyResult(tuple(removed), logical, allocated)


def discard_obsolete_rootfs(cache: Path) -> tuple[str, ...]:
    """Discard only rootfs generations superseded by every current target/profile recipe."""
    removed: list[str] = []
    for entry in _rootfs_entries(cache):
        if entry.action != "candidate":
            continue
        destination = _managed_candidate_directory(cache, entry.path)
        shutil.rmtree(destination)
        removed.append(entry.path)
    return tuple(removed)


def discard_obsolete_apks(cache: Path) -> tuple[str, ...]:
    """Discard only aport cache slots absent from every current target/profile closure."""
    removed: list[str] = []
    for entry in _apk_entries(cache):
        if entry.action != "candidate":
            continue
        destination = _managed_candidate_directory(cache, entry.path)
        shutil.rmtree(destination)
        removed.append(entry.path)
    return tuple(removed)


def _managed_candidate_directory(cache: Path, identity: str) -> Path:
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


def _tree_size(root: Path) -> tuple[int, int]:
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


def _format_sizes(logical: int | None, allocated: int | None) -> str:
    if logical is None or allocated is None:
        return "unmeasured"
    return f"logical={logical} B, allocated={allocated} B"
