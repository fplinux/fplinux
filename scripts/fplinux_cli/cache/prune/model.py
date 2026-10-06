# SPDX-License-Identifier: GPL-2.0-only
"""Managed-cache retention records and command reports."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal


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


def _format_sizes(logical: int | None, allocated: int | None) -> str:
    if logical is None or allocated is None:
        return "unmeasured"
    return f"logical={logical} B, allocated={allocated} B"
