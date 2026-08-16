# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the public workspace prune policy."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from fplinux_cli import prune as prune_module
from fplinux_cli.prune import apply_prune, plan_prune, prune


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


class PruneTests(unittest.TestCase):
    """Exercise the public dry-run and apply policy."""

    def test_superseded_toolchain_is_a_candidate_and_current_is_protected(self) -> None:
        """Only toolchains no current platform references may be removed."""
        current = sorted(prune_module._current_toolchain_recipes())  # noqa: SLF001
        self.assertTrue(current)
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            (cache / "toolchains" / ("0" * 64)).mkdir(parents=True)
            (cache / "toolchains" / ("0" * 64) / "payload").write_bytes(b"stale")
            (cache / "toolchains" / current[0]).mkdir(parents=True)

            plan = plan_prune(cache)
            decisions = {entry.path: entry.action for entry in plan.entries}

            self.assertEqual(decisions[f"toolchains/{'0' * 64}"], "candidate")
            self.assertEqual(decisions[f"toolchains/{current[0]}"], "protected")

            apply_prune(cache)
            self.assertFalse((cache / "toolchains" / ("0" * 64)).exists())
            self.assertTrue((cache / "toolchains" / current[0]).exists())

    def test_ccache_is_inventoried_but_never_auto_removed(self) -> None:
        """The compiler accelerator is reported with its size, not deleted."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            (cache / "ccache").mkdir(parents=True)
            (cache / "ccache" / "blob").write_bytes(b"cached object")

            plan = plan_prune(cache)
            entry = next(item for item in plan.entries if item.path == "ccache")

            self.assertEqual(entry.action, "protected")
            self.assertGreater(entry.logical_bytes or 0, 0)
            apply_prune(cache)
            self.assertTrue((cache / "ccache" / "blob").exists())

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

    def test_unrelated_cache_paths_are_not_inventoried(self) -> None:
        """The workspace pruner ignores every unrelated cache namespace."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            (cache / "unrelated/data").mkdir(parents=True)
            self.assertEqual(plan_prune(cache).entries, ())

    def test_apply_removes_exact_dry_run_candidates(self) -> None:
        """Apply removes exactly the freshly recalculated workspace candidates."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            root = cache / "workspaces"
            generations = [_workspace(root, value) for value in "012345"]
            result = apply_prune(cache)

            self.assertEqual(len(result.removed), 6)
            self.assertTrue(all(not generation.exists() for generation in generations))

    def test_json_output_is_machine_readable(self) -> None:
        """The dry-run JSON contains stable machine-readable summary fields."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            rendered = json.loads(plan_prune(cache).as_json())
            self.assertEqual(rendered["candidate_count"], 0)

    def test_public_prune_prints_without_creating_cache(self) -> None:
        """A dry run against a missing cache remains read-only."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            prune(cache=cache)
            self.assertFalse(cache.exists())


if __name__ == "__main__":
    unittest.main()
