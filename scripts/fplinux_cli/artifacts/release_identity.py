# SPDX-License-Identifier: GPL-2.0-only
"""Release identity shared by host builds and standalone runtimes."""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import TYPE_CHECKING

from fplinux_cli.common import fail, is_sha256

if TYPE_CHECKING:
    from collections.abc import Mapping

RELEASE_FIELDS = frozenset({"release_id", "release_version", "source_commit", "source_snapshot"})
_RELEASE_VERSION = re.compile(r"(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){2}")


@dataclass(frozen=True)
class ReleaseContext:
    """The public version and complete source identity of one build."""

    release_id: str
    release_version: str
    source_commit: str | None
    source_snapshot: str

    @property
    def build_tag(self) -> str:
        """Name a prerelease for this exact build."""
        return f"build-{self.release_id}"

    @property
    def stable_tag(self) -> str:
        """Name the stable release for the declared version."""
        return f"v{self.release_version}"

    def fields(self) -> dict[str, str | None]:
        """Return the release fields carried by build and distribution manifests."""
        return {
            "release_id": self.release_id,
            "release_version": self.release_version,
            "source_commit": self.source_commit,
            "source_snapshot": self.source_snapshot,
        }


def require_release_version(value: object) -> str:
    """Require the canonical public spelling of a stable release version."""
    if not isinstance(value, str) or _RELEASE_VERSION.fullmatch(value) is None:
        fail("release version must use MAJOR.MINOR.PATCH without leading zeroes")
    return value


def stable_tag_version(tag: str) -> tuple[int, ...] | None:
    """Read the numeric version of a canonical stable tag."""
    version = tag.removeprefix("v")
    if tag == version or _RELEASE_VERSION.fullmatch(version) is None:
        return None
    return tuple(int(part) for part in version.split("."))


def release_context_from_fields(fields: Mapping[str, object]) -> ReleaseContext:
    """Read the common release identity from a manifest record."""
    if not RELEASE_FIELDS.issubset(fields):
        fail("release identity is incomplete")
    release_id = fields["release_id"]
    commit = fields["source_commit"]
    snapshot = fields["source_snapshot"]
    version = require_release_version(fields["release_version"])
    if not isinstance(release_id, str) or re.fullmatch(r"[0-9a-f]{12}", release_id) is None:
        fail("release ID must contain twelve lowercase hexadecimal characters")
    if commit is not None and (
        not isinstance(commit, str) or re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit) is None
    ):
        fail("release source commit is invalid")
    if not isinstance(snapshot, str) or not is_sha256(snapshot):
        fail("release source snapshot is invalid")
    if release_id not in {snapshot[:12], commit[:12] if commit is not None else None}:
        fail("release ID does not match its source identity")
    return ReleaseContext(release_id, version, commit, snapshot)
