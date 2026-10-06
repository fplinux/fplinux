# SPDX-License-Identifier: GPL-2.0-only
"""Prune planning for disposable target and quality workspaces."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli.cache.prune.operations import apply_prune, plan_prune

from tests.small.cache.prune_fixtures import _workspace


class WorkspacePruneTests(unittest.TestCase):
    """Exercise prune planning and application on isolated cache trees."""

    def test_dry_run_reports_disposable_workspaces(self) -> None:
        """Every completed target workspace is disposable after its command."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            root = cache / "workspaces"
            generations = [_workspace(root, value) for value in "012345"]
            plan = plan_prune(cache)

            self.assertEqual(
                {entry.path for entry in plan.candidates},
                {f"workspaces/{generation.name}" for generation in generations},
            )
            self.assertGreater(plan.candidate_allocated_bytes, 0)

    def test_quality_workspaces_are_disposable_too(self) -> None:
        """Completed quality snapshots follow the same disposable policy."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            root = cache / "quality-workspaces"
            for value in "0123456789":
                _workspace(root, value, quality=True)
            plan = plan_prune(cache)

            self.assertEqual(len(plan.candidates), 10)

    def test_old_workspace_directory_is_disposable_without_format_support(self) -> None:
        """Delete an old directory without reading or adopting its marker format."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            old = cache / "workspaces/old-format"
            old.mkdir(parents=True)
            (old / "unknown-marker").write_text("ignored\n")

            plan = plan_prune(cache)
            self.assertEqual([entry.path for entry in plan.candidates], ["workspaces/old-format"])
            result = apply_prune(cache)
            self.assertEqual(result.removed, ("workspaces/old-format",))
            self.assertFalse(old.exists())


if __name__ == "__main__":
    unittest.main()
