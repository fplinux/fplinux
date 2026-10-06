# SPDX-License-Identifier: GPL-2.0-only
"""Bounded prune application and preservation of unrelated cache paths."""

from __future__ import annotations

import io
import shutil
from contextlib import redirect_stdout
from typing import TYPE_CHECKING
from unittest import mock

import fplinux_cli.cache.prune.operations as prune_operations
import pytest
from fplinux_cli.cache.prune.model import PruneSafetyError
from fplinux_cli.cache.prune.operations import apply_prune, plan_prune, prune

from tests.small.cache.prune_fixtures import _workspace

if TYPE_CHECKING:
    from pathlib import Path


class PruneApplySafetyTests:
    """Exercise prune planning and application on isolated cache trees."""

    @staticmethod
    def test_apply_refuses_a_toctou_intermediate_symlink(tmp_path: Path) -> None:
        """A candidate planned before replacement cannot delete through a later cache link."""
        cache = tmp_path / ".cache"
        candidate = cache / "workspaces/stale"
        candidate.mkdir(parents=True)
        (candidate / "payload").write_text("stale\n")
        plan = plan_prune(cache)
        external = tmp_path / "external"
        external.mkdir()
        sentinel = external / "sentinel"
        sentinel.write_text("keep\n")
        shutil.rmtree(cache / "workspaces")
        (cache / "workspaces").symlink_to(external, target_is_directory=True)

        with (
            mock.patch.object(prune_operations, "plan_prune", return_value=plan),
            pytest.raises(PruneSafetyError, match="not a real directory"),
        ):
            apply_prune(cache)
        assert sentinel.exists()

    @staticmethod
    def test_unrelated_cache_paths_are_not_inventoried(tmp_path: Path) -> None:
        """The workspace pruner ignores every unrelated cache namespace."""
        cache = tmp_path / ".cache"
        (cache / "unrelated/data").mkdir(parents=True)
        assert (plan_prune(cache).entries) == (())

    @staticmethod
    def test_apply_removes_disposable_workspaces(tmp_path: Path) -> None:
        """Apply removes exactly the freshly recalculated workspace candidates."""
        cache = tmp_path / ".cache"
        root = cache / "workspaces"
        generations = [_workspace(root, value) for value in "012345"]
        expected = tuple(f"workspaces/{generation.name}" for generation in generations)
        result = apply_prune(cache)

        assert (result.removed) == (expected)
        assert all(not generation.exists() for generation in generations)

    @staticmethod
    def test_prune_function_prints_exact_empty_text_dry_run(tmp_path: Path) -> None:
        """The text mode writes the exact empty dry-run report."""
        cache = tmp_path / ".cache"
        output = io.StringIO()
        with redirect_stdout(output):
            prune(cache=cache)

        assert (output.getvalue()) == (
            "prune: dry-run; no cache changes will be made\n"
            "summary: 0 candidates; logical=0 B; allocated=0 B\n"
        )
        assert not (cache.exists())
