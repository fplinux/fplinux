# SPDX-License-Identifier: GPL-2.0-only
"""Tests for workspace prune planning and application."""

from __future__ import annotations

import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from fplinux_cli import alpine_state
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


def _cli_log(cache: Path, command: str, sequence: int, *, target: str | None = None) -> Path:
    run_id = f"20260820T0508{sequence:02d}Z-p{sequence + 100}"
    relative = Path("logs") / command
    label = command
    if target is not None:
        relative /= target
        label = f"{command} {target}"
    relative /= run_id
    path = cache / relative
    path.mkdir(parents=True)
    (path / "run.json").write_text(
        json.dumps(
            {
                "display_root": f".cache/{relative.as_posix()}",
                "label": label,
                "parent": None,
            }
        )
    )
    (path / "stage.log").write_text("log\n")
    return path


class PruneTests(unittest.TestCase):
    """Exercise prune planning and application on isolated cache trees."""

    def test_superseded_rootfs_is_a_candidate_and_all_current_are_protected(self) -> None:
        """Every current target package selection protects its rootfs recipe."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            public_key = alpine_state.signing_public_key(cache)
            public_key.parent.mkdir(parents=True)
            public_key.write_bytes(b"public-key\n")
            first_packages = ("package-a",)
            second_packages = ("package-a", "package-b")
            first_recipe = "1" * 64
            second_recipe = "2" * 64
            current = {first_recipe, second_recipe}
            stale = cache / "rootfs" / ("0" * 64)
            stale.mkdir(parents=True)
            (stale / "rootfs.cpio").write_bytes(b"stale")
            for recipe in current:
                (cache / "rootfs" / recipe).mkdir(parents=True)

            target_configs = {
                "first": {"platform": "platform-a"},
                "second": {"platform": "platform-b"},
            }
            platform_configs: dict[str, dict[str, object]] = {
                "platform-a": {},
                "platform-b": {},
            }

            def rootfs_recipe(
                _image_recipe: str,
                _signing_key: str,
                packages: tuple[str, ...],
            ) -> str:
                return {
                    first_packages: first_recipe,
                    second_packages: second_recipe,
                }[packages]

            with (
                mock.patch.object(
                    prune_module,
                    "discover_targets",
                    return_value=("first", "second"),
                ),
                mock.patch.object(
                    prune_module,
                    "load_target",
                    side_effect=lambda target: target_configs[target],
                ),
                mock.patch.object(
                    prune_module,
                    "load_platform",
                    side_effect=lambda platform: platform_configs[platform],
                ),
                mock.patch.object(
                    alpine_state,
                    "selected_packages",
                    side_effect=(first_packages, second_packages),
                ),
                mock.patch.object(
                    alpine_state,
                    "alpine_rootfs_recipe",
                    side_effect=rootfs_recipe,
                ),
                mock.patch.object(
                    prune_module,
                    "container_image_recipe_digest",
                    return_value="a" * 64,
                ),
            ):
                plan = plan_prune(cache)
            decisions = {entry.path: entry.action for entry in plan.entries}

            self.assertEqual(decisions[f"rootfs/{'0' * 64}"], "candidate")
            self.assertEqual(
                {path for path, action in decisions.items() if action == "protected"},
                {f"rootfs/{recipe}" for recipe in current},
            )
            with (
                mock.patch.object(
                    prune_module,
                    "discover_targets",
                    return_value=("first", "second"),
                ),
                mock.patch.object(
                    prune_module,
                    "load_target",
                    side_effect=lambda target: target_configs[target],
                ),
                mock.patch.object(
                    prune_module,
                    "load_platform",
                    side_effect=lambda platform: platform_configs[platform],
                ),
                mock.patch.object(
                    alpine_state,
                    "selected_packages",
                    side_effect=(first_packages, second_packages),
                ),
                mock.patch.object(
                    alpine_state,
                    "alpine_rootfs_recipe",
                    side_effect=rootfs_recipe,
                ),
                mock.patch.object(
                    prune_module,
                    "container_image_recipe_digest",
                    return_value="a" * 64,
                ),
            ):
                apply_prune(cache)
            self.assertFalse(stale.exists())
            self.assertTrue(all((cache / "rootfs" / recipe).exists() for recipe in current))

    def test_missing_signing_key_protects_existing_rootfs(self) -> None:
        """Never prune rootfs generations when their package-signing input is unknown."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            existing = cache / "rootfs" / ("1" * 64)
            existing.mkdir(parents=True)
            plan = plan_prune(cache)
            self.assertEqual(len(plan.entries), 1)
            self.assertEqual(plan.entries[0].action, "protected")
            self.assertIn("rootfs recipes", plan.entries[0].reason)

    def test_config_or_selection_failure_protects_existing_rootfs(self) -> None:
        """An incomplete target inventory never makes an existing rootfs disposable."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            public_key = alpine_state.signing_public_key(cache)
            public_key.parent.mkdir(parents=True)
            public_key.write_bytes(b"public-key\n")
            existing = cache / "rootfs" / ("2" * 64)
            existing.mkdir(parents=True)

            with mock.patch.object(
                prune_module, "discover_targets", side_effect=SystemExit("bad target manifest")
            ):
                plan = plan_prune(cache)

            self.assertEqual(len(plan.entries), 1)
            self.assertEqual(plan.entries[0].action, "protected")
            self.assertIn("rootfs recipes", plan.entries[0].reason)

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

    def test_cli_log_retention_keeps_the_newest_runs_per_command_and_target(self) -> None:
        """Keep ten generated check/setup logs and ten builds for every target."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            check_runs = [_cli_log(cache, "check", sequence) for sequence in range(12)]
            setup_runs = [_cli_log(cache, "setup", sequence) for sequence in range(12)]
            first_target_runs = [
                _cli_log(cache, "build", sequence, target="target-a") for sequence in range(11)
            ]
            second_target_runs = [
                _cli_log(cache, "build", sequence, target="target-b") for sequence in range(11)
            ]

            plan = plan_prune(cache)

            self.assertEqual(
                {entry.path for entry in plan.candidates},
                {
                    *(f"logs/check/{run.name}" for run in check_runs[:2]),
                    *(f"logs/setup/{run.name}" for run in setup_runs[:2]),
                    f"logs/build/target-a/{first_target_runs[0].name}",
                    f"logs/build/target-b/{second_target_runs[0].name}",
                },
            )
            protected = {entry.path for entry in plan.entries if entry.action == "protected"}
            self.assertEqual(len(protected), 40)
            self.assertIn(f"logs/check/{check_runs[-1].name}", protected)
            self.assertIn(f"logs/setup/{setup_runs[-1].name}", protected)
            self.assertIn(f"logs/build/target-a/{first_target_runs[-1].name}", protected)
            self.assertIn(f"logs/build/target-b/{second_target_runs[-1].name}", protected)

    def test_log_apply_removes_only_old_generated_nested_paths(self) -> None:
        """Ignore manual, unknown, and mismatched log directories during apply."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            check_runs = [_cli_log(cache, "check", sequence) for sequence in range(11)]
            build_runs = [
                _cli_log(cache, "build", sequence, target="target-a") for sequence in range(11)
            ]
            manual = cache / "logs/manual/keep-this"
            unknown_namespace = cache / "logs/imported/keep-this"
            mismatched = cache / "logs/check/20260820T060000Z-p999"
            for path in (manual, unknown_namespace, mismatched):
                path.mkdir(parents=True)
                (path / "note").write_text("keep\n")

            result = apply_prune(cache)

            self.assertEqual(
                result.removed,
                (
                    f"logs/build/target-a/{build_runs[0].name}",
                    f"logs/check/{check_runs[0].name}",
                ),
            )
            self.assertFalse(check_runs[0].exists())
            self.assertFalse(build_runs[0].exists())
            self.assertTrue(check_runs[-1].exists())
            self.assertTrue(build_runs[-1].exists())
            self.assertTrue(manual.exists())
            self.assertTrue(unknown_namespace.exists())
            self.assertTrue(mismatched.exists())

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
            expected = tuple(f"workspaces/{generation.name}" for generation in generations)
            result = apply_prune(cache)

            self.assertEqual(result.removed, expected)
            self.assertTrue(all(not generation.exists() for generation in generations))

    def test_prune_function_prints_exact_empty_json_dry_run(self) -> None:
        """The JSON mode writes the exact empty dry-run document."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            output = io.StringIO()
            with redirect_stdout(output):
                prune(cache=cache, json_output=True)

            self.assertEqual(
                output.getvalue(),
                "{\n"
                '  "candidate_allocated_bytes": 0,\n'
                '  "candidate_count": 0,\n'
                '  "candidate_logical_bytes": 0,\n'
                '  "entries": [],\n'
                '  "mode": "dry-run",\n'
                '  "unsafe": []\n'
                "}\n",
            )
            self.assertFalse(cache.exists())

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
