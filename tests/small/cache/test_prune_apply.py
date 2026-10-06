# SPDX-License-Identifier: GPL-2.0-only
"""Bounded prune application and preservation of unrelated cache paths."""

from __future__ import annotations

import io
import shutil
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

import fplinux_cli.cache.prune.operations as prune_operations
from fplinux_cli.cache.prune.model import PruneSafetyError
from fplinux_cli.cache.prune.operations import apply_prune, plan_prune, prune

from tests.small.cache.prune_fixtures import _workspace


class PruneApplySafetyTests(unittest.TestCase):
    """Exercise prune planning and application on isolated cache trees."""

    def test_apply_refuses_a_toctou_intermediate_symlink(self) -> None:
        """A candidate planned before replacement cannot delete through a later cache link."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            candidate = cache / "workspaces/stale"
            candidate.mkdir(parents=True)
            (candidate / "payload").write_text("stale\n")
            plan = plan_prune(cache)
            external = Path(temporary) / "external"
            external.mkdir()
            sentinel = external / "sentinel"
            sentinel.write_text("keep\n")
            shutil.rmtree(cache / "workspaces")
            (cache / "workspaces").symlink_to(external, target_is_directory=True)

            with (
                mock.patch.object(prune_operations, "plan_prune", return_value=plan),
                self.assertRaisesRegex(PruneSafetyError, "not a real directory"),
            ):
                apply_prune(cache)
            self.assertTrue(sentinel.exists())

    def test_unrelated_cache_paths_are_not_inventoried(self) -> None:
        """The workspace pruner ignores every unrelated cache namespace."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            (cache / "unrelated/data").mkdir(parents=True)
            self.assertEqual(plan_prune(cache).entries, ())

    def test_apply_removes_disposable_workspaces(self) -> None:
        """Apply removes exactly the freshly recalculated workspace candidates."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            root = cache / "workspaces"
            generations = [_workspace(root, value) for value in "012345"]
            expected = tuple(f"workspaces/{generation.name}" for generation in generations)
            result = apply_prune(cache)

            self.assertEqual(result.removed, expected)
            self.assertTrue(all(not generation.exists() for generation in generations))

    def test_prune_function_prints_exact_empty_text_dry_run(self) -> None:
        """The text mode writes the exact empty dry-run report."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            output = io.StringIO()
            with redirect_stdout(output):
                prune(cache=cache)

            self.assertEqual(
                output.getvalue(),
                "prune: dry-run; no cache changes will be made\n"
                "summary: 0 candidates; logical=0 B; allocated=0 B\n",
            )
            self.assertFalse(cache.exists())


if __name__ == "__main__":
    unittest.main()
