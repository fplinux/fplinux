# SPDX-License-Identifier: GPL-2.0-only
"""Completed workspace trees shared by prune scenarios."""

from __future__ import annotations

from contextlib import contextmanager
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli.cache.prune import alpine, profiles

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


def _workspace(root: Path, name: str, *, quality: bool = False) -> Path:
    digest = name * 64
    path = root / digest
    path.mkdir(parents=True)
    marker = path / (".cache/.fplinux-workspace" if quality else ".fplinux-workspace")
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(digest + "\n")
    (path / "payload").write_bytes(b"payload")
    path.touch()
    path.chmod(0o755)
    return path


@contextmanager
def patch_prune_discovery(
    name: str,
    *,
    return_value: tuple[str, ...] = (),
    side_effect: BaseException | None = None,
) -> Iterator[None]:
    """Control target discovery in both resource callers of one prune plan."""
    with (
        mock.patch.object(alpine, name, return_value=return_value, side_effect=side_effect),
        mock.patch.object(profiles, name, return_value=return_value, side_effect=side_effect),
    ):
        yield
