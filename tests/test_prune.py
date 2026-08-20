# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the public workspace prune policy."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import alpine_state
from fplinux_cli import prune as prune_module
from fplinux_cli.config import container_image_recipe_digest
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
    """Exercise the public dry-run and apply policy."""

    def test_superseded_rootfs_is_a_candidate_and_all_current_are_protected(self) -> None:
        """Every current target package selection protects its rootfs recipe."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            public_key = alpine_state.signing_public_key(cache)
            public_key.parent.mkdir(parents=True)
            public_key.write_bytes(b"public-key\n")
            signing_key = alpine_state.signing_key_identity(cache)
            image_recipe = container_image_recipe_digest()
            first_packages = ("fplinux-base",)
            second_packages = ("fplinux-base", "fplinux-cpuclock")
            current = {
                alpine_state.alpine_rootfs_recipe(image_recipe, signing_key, first_packages),
                alpine_state.alpine_rootfs_recipe(image_recipe, signing_key, second_packages),
            }
            self.assertEqual(len(current), 2)
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
                ) as selected_packages,
            ):
                plan = plan_prune(cache)
            decisions = {entry.path: entry.action for entry in plan.entries}

            self.assertEqual(decisions[f"rootfs/{'0' * 64}"], "candidate")
            self.assertEqual(
                {path for path, action in decisions.items() if action == "protected"},
                {f"rootfs/{recipe}" for recipe in current},
            )
            self.assertEqual(
                selected_packages.call_args_list,
                [
                    mock.call(platform_configs["platform-a"], target_configs["first"]),
                    mock.call(platform_configs["platform-b"], target_configs["second"]),
                ],
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

    def test_legacy_cache_namespaces_are_not_adopted(self) -> None:
        """Obsolete compiler caches are no longer managed automatically."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            (cache / "ccache").mkdir(parents=True)
            (cache / "toolchains/legacy").mkdir(parents=True)

            self.assertEqual(plan_prune(cache).entries, ())

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
            inoi_runs = [
                _cli_log(cache, "build", sequence, target="inoi-244-modern-4g")
                for sequence in range(11)
            ]
            nokia_runs = [
                _cli_log(cache, "build", sequence, target="nokia-ta1618") for sequence in range(11)
            ]

            plan = plan_prune(cache)

            self.assertEqual(
                {entry.path for entry in plan.candidates},
                {
                    *(f"logs/check/{run.name}" for run in check_runs[:2]),
                    *(f"logs/setup/{run.name}" for run in setup_runs[:2]),
                    f"logs/build/inoi-244-modern-4g/{inoi_runs[0].name}",
                    f"logs/build/nokia-ta1618/{nokia_runs[0].name}",
                },
            )
            protected = {entry.path for entry in plan.entries if entry.action == "protected"}
            self.assertEqual(len(protected), 40)
            self.assertIn(f"logs/check/{check_runs[-1].name}", protected)
            self.assertIn(f"logs/setup/{setup_runs[-1].name}", protected)
            self.assertIn(f"logs/build/inoi-244-modern-4g/{inoi_runs[-1].name}", protected)
            self.assertIn(f"logs/build/nokia-ta1618/{nokia_runs[-1].name}", protected)

    def test_log_apply_removes_only_old_generated_nested_paths(self) -> None:
        """Ignore manual, unknown, and mismatched log directories during apply."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            check_runs = [_cli_log(cache, "check", sequence) for sequence in range(11)]
            build_runs = [
                _cli_log(cache, "build", sequence, target="inoi-244-modern-4g")
                for sequence in range(11)
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
                    f"logs/build/inoi-244-modern-4g/{build_runs[0].name}",
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
