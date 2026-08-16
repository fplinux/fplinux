# SPDX-License-Identifier: GPL-2.0-only
"""List and remove stale immutable FPLinux source workspaces."""

from __future__ import annotations

import json
import shutil
from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal

from .common import ROOT
from .config import container_image_recipe_digest, load_container_lock, load_platform
from .toolchain_state import toolchain_recipe

if TYPE_CHECKING:
    from pathlib import Path

_WORKSPACE_NAMESPACES = frozenset({"quality-workspaces", "workspaces"})
_MANAGED_NAMESPACES = _WORKSPACE_NAMESPACES | {"toolchains"}


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


def _current_toolchain_recipes() -> frozenset[str]:
    """Return the toolchain digests every declared platform references now."""
    lock = load_container_lock()
    image_recipe = container_image_recipe_digest(lock)
    recipes: set[str] = set()
    for manifest in sorted((ROOT / "platforms").glob("*/platform.toml")):
        platform = load_platform(manifest.parent.name)
        if platform["buildroot"].get("toolchain_defconfig"):
            recipes.add(toolchain_recipe(ROOT, platform, lock, image_recipe))
    return frozenset(recipes)


def _toolchain_entries(cache: Path) -> list[InventoryEntry]:
    toolchains = cache / "toolchains"
    if not toolchains.is_dir():
        return []
    current = _current_toolchain_recipes()
    entries: list[InventoryEntry] = []
    for path in sorted(toolchains.iterdir(), key=lambda item: item.name):
        identity = f"toolchains/{path.name}"
        if not path.is_dir() or path.is_symlink():
            entries.append(
                InventoryEntry(identity, "protected", "not a toolchain directory", None, None)
            )
        elif path.name in current:
            entries.append(
                InventoryEntry(identity, "protected", "current shared toolchain", None, None)
            )
        else:
            logical, allocated = _tree_size(path)
            entries.append(
                InventoryEntry(
                    identity,
                    "candidate",
                    "superseded shared toolchain",
                    logical,
                    allocated,
                )
            )
    return entries


def _ccache_entry(cache: Path) -> list[InventoryEntry]:
    ccache = cache / "ccache"
    if not ccache.is_dir():
        return []
    logical, allocated = _tree_size(ccache)
    return [
        InventoryEntry(
            "ccache",
            "protected",
            "compiler accelerator; delete manually to reclaim space",
            logical,
            allocated,
        )
    ]


def plan_prune(cache: Path) -> PrunePlan:
    """List disposable snapshots in every managed cache namespace."""
    entries: list[InventoryEntry] = []
    entries.extend(_toolchain_entries(cache))
    entries.extend(_ccache_entry(cache))
    for namespace in sorted(_WORKSPACE_NAMESPACES):
        root = cache / namespace
        if not root.is_dir():
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


def apply_prune(cache: Path) -> PruneApplyResult:
    """Delete only candidates from a fresh plan while the CLI holds its global lock."""
    plan = plan_prune(cache)
    removed: list[str] = []
    logical = 0
    allocated = 0
    for entry in plan.candidates:
        namespace, name = entry.path.split("/", 1)
        if namespace not in _MANAGED_NAMESPACES or "/" in name or not name:
            raise PruneSafetyError(f"invalid managed candidate: {entry.path}")
        destination = cache / namespace / name
        if destination.is_dir():
            shutil.rmtree(destination)
        removed.append(entry.path)
        logical += entry.logical_bytes or 0
        allocated += entry.allocated_bytes or 0
    return PruneApplyResult(tuple(removed), logical, allocated)


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
