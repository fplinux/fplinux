# SPDX-License-Identifier: GPL-2.0-only
"""Prune planning for disposable target and quality workspaces."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.cache.prune.operations import apply_prune, plan_prune

from tests.small.cache.prune_fixtures import _workspace

if TYPE_CHECKING:
    from pathlib import Path


class WorkspacePruneTests:
    """Exercise prune planning and application on isolated cache trees."""

    @staticmethod
    def test_dry_run_reports_disposable_workspaces(tmp_path: Path) -> None:
        """Every completed target workspace is disposable after its command."""
        cache = tmp_path / ".cache"
        root = cache / "workspaces"
        generations = [_workspace(root, value) for value in "012345"]
        plan = plan_prune(cache)

        assert ({entry.path for entry in plan.candidates}) == (
            {f"workspaces/{generation.name}" for generation in generations}
        )
        assert (plan.candidate_allocated_bytes) > (0)

    @staticmethod
    def test_quality_workspaces_are_disposable_too(tmp_path: Path) -> None:
        """Completed quality snapshots follow the same disposable policy."""
        cache = tmp_path / ".cache"
        root = cache / "quality-workspaces"
        for value in "0123456789":
            _workspace(root, value, quality=True)
        plan = plan_prune(cache)

        assert (len(plan.candidates)) == (10)

    @staticmethod
    def test_old_workspace_directory_is_disposable_without_format_support(tmp_path: Path) -> None:
        """Delete an old directory without reading or adopting its marker format."""
        cache = tmp_path / ".cache"
        old = cache / "workspaces/old-format"
        old.mkdir(parents=True)
        (old / "unknown-marker").write_text("ignored\n")

        plan = plan_prune(cache)
        assert ([entry.path for entry in plan.candidates]) == (["workspaces/old-format"])
        result = apply_prune(cache)
        assert (result.removed) == (("workspaces/old-format",))
        assert not (old.exists())
