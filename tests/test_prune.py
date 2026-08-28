# SPDX-License-Identifier: GPL-2.0-only
"""Tests for workspace prune planning and application."""

from __future__ import annotations

import io
import json
import shutil
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from fplinux_cli import alpine_state
from fplinux_cli import prune as prune_module
from fplinux_cli.prune import PruneSafetyError, apply_prune, plan_prune, prune


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
                mock.patch.object(prune_module, "discover_profiles", return_value=()),
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
                mock.patch.object(prune_module, "discover_profiles", return_value=()),
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

    def test_current_rootfs_recipes_include_each_declared_profile(self) -> None:
        """One profile-only rootfs remains protected even when default differs."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            public_key = alpine_state.signing_public_key(cache)
            public_key.parent.mkdir(parents=True)
            public_key.write_bytes(b"public-key\n")
            default_recipe = "3" * 64
            profile_recipe = "4" * 64
            for recipe in (default_recipe, profile_recipe):
                (cache / "rootfs" / recipe).mkdir(parents=True)

            def target_config(_target: str, profile: str | None = None) -> dict[str, object]:
                return {"platform": "platform", "profile": profile}

            def selected_packages(
                _platform: dict[str, object], config: dict[str, object]
            ) -> tuple[str, ...]:
                return ("package-host",) if config["profile"] == "host" else ("package-base",)

            def rootfs_recipe(_image: str, _key: str, packages: tuple[str, ...]) -> str:
                recipes: dict[tuple[str, ...], str] = {
                    ("package-base",): default_recipe,
                    ("package-host",): profile_recipe,
                }
                return recipes[packages]

            with (
                mock.patch.object(prune_module, "discover_targets", return_value=("phone",)),
                mock.patch.object(prune_module, "discover_profiles", return_value=("host",)),
                mock.patch.object(prune_module, "load_target", side_effect=target_config),
                mock.patch.object(prune_module, "load_platform", return_value={}),
                mock.patch.object(
                    alpine_state,
                    "selected_packages",
                    side_effect=selected_packages,
                ),
                mock.patch.object(alpine_state, "alpine_rootfs_recipe", side_effect=rootfs_recipe),
                mock.patch.object(
                    prune_module,
                    "container_image_recipe_digest",
                    return_value="a" * 64,
                ),
            ):
                plan = plan_prune(cache)

            decisions = {entry.path: entry.action for entry in plan.entries}
            self.assertEqual(decisions[f"rootfs/{default_recipe}"], "protected")
            self.assertEqual(decisions[f"rootfs/{profile_recipe}"], "protected")

    def test_build_cleanup_removes_only_superseded_rootfs_generations(self) -> None:
        """A successful build can bound rootfs state without invoking broad prune."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            public_key = alpine_state.signing_public_key(cache)
            public_key.parent.mkdir(parents=True)
            public_key.write_bytes(b"public-key\n")
            current = "5" * 64
            stale = "6" * 64
            for recipe in (current, stale):
                (cache / "rootfs" / recipe).mkdir(parents=True)

            with (
                mock.patch.object(prune_module, "discover_targets", return_value=("phone",)),
                mock.patch.object(prune_module, "discover_profiles", return_value=()),
                mock.patch.object(
                    prune_module,
                    "load_target",
                    return_value={"platform": "platform"},
                ),
                mock.patch.object(prune_module, "load_platform", return_value={}),
                mock.patch.object(
                    alpine_state,
                    "selected_packages",
                    return_value=("package",),
                ),
                mock.patch.object(alpine_state, "alpine_rootfs_recipe", return_value=current),
                mock.patch.object(
                    prune_module,
                    "container_image_recipe_digest",
                    return_value="a" * 64,
                ),
            ):
                removed = prune_module.discard_obsolete_rootfs(cache)

            self.assertEqual(removed, (f"rootfs/{stale}",))
            self.assertTrue((cache / "rootfs" / current).exists())
            self.assertFalse((cache / "rootfs" / stale).exists())

    def test_build_rootfs_cleanup_leaves_a_symlinked_entry_untouched(self) -> None:
        """Automatic retention never follows or removes an unsafe rootfs cache link."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            public_key = alpine_state.signing_public_key(cache)
            public_key.parent.mkdir(parents=True)
            public_key.write_bytes(b"public-key\n")
            outside = Path(temporary) / "outside"
            outside.mkdir()
            linked = cache / "rootfs" / ("7" * 64)
            linked.parent.mkdir(parents=True)
            linked.symlink_to(outside, target_is_directory=True)

            with mock.patch.object(
                prune_module,
                "discover_targets",
                side_effect=SystemExit("unavailable"),
            ):
                self.assertEqual(prune_module.discard_obsolete_rootfs(cache), ())
            self.assertTrue(linked.is_symlink())
            self.assertTrue(outside.exists())

    def test_profile_package_replacement_prunes_only_the_obsolete_aport_slot(self) -> None:
        """Replacing a profile package declaration leaves its former cache slot disposable."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            apks = cache / alpine_state.PACKAGE_CACHE_DIRECTORY
            current = {
                "fplinux-base",
                "fplinux-bundle-base",
                "fplinux-profile-y",
                "fplinux-bundle-host",
            }
            obsolete = "fplinux-profile-x"
            for package in (*current, obsolete):
                directory = apks / package
                directory.mkdir(parents=True)
                (directory / "payload").write_text(package + "\n")

            external = Path(temporary) / "external"
            external.mkdir()
            link = apks / "fplinux-link"
            link.symlink_to(external, target_is_directory=True)
            regular = apks / "fplinux-file"
            regular.write_text("keep\n")

            def load_target(_target: str, profile: str | None = None) -> dict[str, object]:
                return {"platform": "platform", "profile": profile}

            def selected_packages(
                _platform: dict[str, object], config: dict[str, object]
            ) -> tuple[str, ...]:
                return ("fplinux-profile-y",) if config["profile"] == "host" else ("fplinux-base",)

            def bundle_packages(
                _platform: dict[str, object], config: dict[str, object], _rootfs: tuple[str, ...]
            ) -> tuple[str, ...]:
                return (
                    ("fplinux-bundle-host",)
                    if config["profile"] == "host"
                    else ("fplinux-bundle-base",)
                )

            with (
                mock.patch.object(prune_module, "discover_targets", return_value=("phone",)),
                mock.patch.object(prune_module, "discover_profiles", return_value=("host",)),
                mock.patch.object(prune_module, "load_target", side_effect=load_target),
                mock.patch.object(prune_module, "load_platform", return_value={}),
                mock.patch.object(
                    alpine_state,
                    "selected_packages",
                    side_effect=selected_packages,
                ),
                mock.patch.object(
                    alpine_state,
                    "bundle_packages",
                    side_effect=bundle_packages,
                ),
            ):
                plan = plan_prune(cache)
                result = apply_prune(cache)

            decisions = {entry.path: entry.action for entry in plan.entries}
            self.assertEqual(decisions[f"apks/{obsolete}"], "candidate")
            for package in current:
                self.assertEqual(decisions[f"apks/{package}"], "protected")
            self.assertEqual(decisions["apks/fplinux-link"], "protected")
            self.assertEqual(decisions["apks/fplinux-file"], "protected")
            self.assertEqual(result.removed, (f"apks/{obsolete}",))
            self.assertFalse((apks / obsolete).exists())
            self.assertTrue(all((apks / package).is_dir() for package in current))
            self.assertTrue(link.is_symlink())
            self.assertTrue(regular.is_file())

    def test_package_config_failure_protects_every_aport_cache_slot(self) -> None:
        """An unreadable target/profile closure cannot make package output disposable."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            apks = cache / alpine_state.PACKAGE_CACHE_DIRECTORY
            for package in ("fplinux-base", "fplinux-obsolete"):
                (apks / package).mkdir(parents=True)

            with mock.patch.object(
                prune_module,
                "discover_targets",
                side_effect=SystemExit("bad target manifest"),
            ):
                plan = plan_prune(cache)

            self.assertEqual(
                {entry.path: entry.action for entry in plan.entries},
                {
                    "apks/fplinux-base": "protected",
                    "apks/fplinux-obsolete": "protected",
                },
            )
            self.assertTrue(
                all("closure is unavailable" in entry.reason for entry in plan.entries)
            )

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
                mock.patch.object(prune_module, "plan_prune", return_value=plan),
                self.assertRaisesRegex(PruneSafetyError, "not a real directory"),
            ):
                apply_prune(cache)
            self.assertTrue(sentinel.exists())

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

    def test_prune_removes_only_an_orphaned_profile_cache_slot(self) -> None:
        """A removed profile cannot retain a stable Kbuild work directory indefinitely."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            current = cache / "out/nokia/profiles/usb-host-lab"
            orphan = cache / "out/nokia/profiles/removed-profile"
            for path in (current, orphan):
                path.mkdir(parents=True)
                (path / "work").mkdir()
                (path / "work/output").write_text("generated\n")

            with (
                mock.patch.object(prune_module, "discover_targets", return_value=("nokia",)),
                mock.patch.object(
                    prune_module,
                    "discover_profiles",
                    return_value=("usb-host-lab",),
                ),
            ):
                plan = plan_prune(cache)
                result = apply_prune(cache)

            decisions = {entry.path: entry.action for entry in plan.entries}
            self.assertEqual(decisions["out/nokia/profiles/usb-host-lab"], "protected")
            self.assertEqual(decisions["out/nokia/profiles/removed-profile"], "candidate")
            self.assertEqual(result.removed, ("out/nokia/profiles/removed-profile",))
            self.assertTrue(current.exists())
            self.assertFalse(orphan.exists())

    def test_prune_removes_only_an_orphaned_profile_check_receipt(self) -> None:
        """A removed profile cannot retain a separate kernel check receipt."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            current = cache / "check-results/profiles/usb-host-lab/kernel"
            orphan = cache / "check-results/profiles/removed-profile/kernel"
            for path in (current, orphan):
                path.mkdir(parents=True)
                (path / "success.json").write_text("generated\n")

            with (
                mock.patch.object(prune_module, "discover_targets", return_value=("nokia",)),
                mock.patch.object(
                    prune_module,
                    "discover_profiles",
                    return_value=("usb-host-lab",),
                ),
            ):
                plan = plan_prune(cache)
                result = apply_prune(cache)

            decisions = {entry.path: entry.action for entry in plan.entries}
            self.assertEqual(
                decisions["check-results/profiles/usb-host-lab"],
                "protected",
            )
            self.assertEqual(
                decisions["check-results/profiles/removed-profile"],
                "candidate",
            )
            self.assertEqual(result.removed, ("check-results/profiles/removed-profile",))
            self.assertTrue(current.exists())
            self.assertFalse(orphan.exists())

    def test_prune_removes_orphaned_profile_slots_after_a_target_is_deleted(self) -> None:
        """No profile-only namespace can retain a removed target's generated tree."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            paths = (
                cache / "out/deleted/profiles/host",
                cache / "linux/profiles/deleted/host",
                cache / "analysis/sparse/deleted/profiles/host",
            )
            for path in paths:
                path.mkdir(parents=True)
                (path / "generated").write_text("generated\n")

            with (
                mock.patch.object(prune_module, "discover_targets", return_value=("current",)),
                mock.patch.object(prune_module, "discover_profiles", return_value=()),
            ):
                result = apply_prune(cache)

            self.assertEqual(
                result.removed,
                (
                    "analysis/sparse/deleted/profiles/host",
                    "linux/profiles/deleted/host",
                    "out/deleted/profiles/host",
                ),
            )
            self.assertTrue(all(not path.exists() for path in paths))

    def test_profile_linux_slot_is_disposable_after_it_reuses_the_default_source(self) -> None:
        """Removing a profile patch cannot leave its former prepared Linux tree behind."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            slot = cache / "linux/profiles/phone/host"
            slot.mkdir(parents=True)
            (slot / "prepared").write_text("generated\n")
            default: dict[str, object] = {
                "linux": {
                    "patches": [],
                    "copies": [],
                    "appends": [],
                    "root": {"kind": "initramfs"},
                }
            }
            profile: dict[str, object] = {
                "linux": {
                    "patches": [],
                    "copies": [],
                    "appends": [],
                    "root": {"kind": "initramfs"},
                }
            }

            def load_target(_target: str, selected: str | None = None) -> dict[str, object]:
                return profile if selected is not None else default

            with (
                mock.patch.object(prune_module, "discover_targets", return_value=("phone",)),
                mock.patch.object(prune_module, "discover_profiles", return_value=("host",)),
                mock.patch.object(prune_module, "load_target", side_effect=load_target),
            ):
                result = apply_prune(cache)

            self.assertEqual(result.removed, ("linux/profiles/phone/host",))
            self.assertFalse(slot.exists())

    def test_external_root_profile_keeps_its_generated_linux_tree(self) -> None:
        """Generated external-root bootargs require a dedicated prepared source."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            slot = cache / "linux/profiles/phone/microsd"
            slot.mkdir(parents=True)
            (slot / "prepared").write_text("generated\n")
            base_linux: dict[str, object] = {"patches": [], "copies": [], "appends": []}
            default: dict[str, object] = {"linux": {**base_linux, "root": {"kind": "initramfs"}}}
            profile: dict[str, object] = {
                "linux": {
                    **base_linux,
                    "root": {
                        "kind": "external",
                        "filesystem": "ext4",
                        "partuuid": "46504c58-02",
                        "wait_seconds": 10,
                    },
                }
            }

            def load_target(_target: str, selected: str | None = None) -> dict[str, object]:
                return profile if selected is not None else default

            with (
                mock.patch.object(prune_module, "discover_targets", return_value=("phone",)),
                mock.patch.object(prune_module, "discover_profiles", return_value=("microsd",)),
                mock.patch.object(prune_module, "load_target", side_effect=load_target),
            ):
                plan = plan_prune(cache)

            entry = next(
                item for item in plan.entries if item.path == "linux/profiles/phone/microsd"
            )
            self.assertEqual(entry.action, "protected")
            self.assertEqual(entry.reason, "declared profile cache")
            self.assertTrue(slot.exists())

    def test_fixed_linux_staging_slots_are_disposable(self) -> None:
        """Interrupted preparation leaves only exact staging paths that prune may remove."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            default = cache / "linux/staging/phone/default"
            profile = cache / "linux/staging/phone/profiles/host"
            for path in (default, profile):
                path.mkdir(parents=True)
                (path / "partial").write_text("partial\n")

            result = apply_prune(cache)

            self.assertEqual(
                result.removed,
                (
                    "linux/staging/phone/default",
                    "linux/staging/phone/profiles/host",
                ),
            )
            self.assertFalse(default.exists())
            self.assertFalse(profile.exists())

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

    def test_profile_logs_have_the_same_bounded_retention(self) -> None:
        """Named profile logs cannot accumulate beyond the normal per-slot limit."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            check_runs = [
                _cli_log(cache, "check", sequence, target="profiles/usb-host-lab")
                for sequence in range(11)
            ]
            build_runs = [
                _cli_log(cache, "build", sequence, target="nokia/profiles/usb-host-lab")
                for sequence in range(11)
            ]

            with (
                mock.patch.object(prune_module, "discover_targets", return_value=("nokia",)),
                mock.patch.object(
                    prune_module,
                    "discover_profiles",
                    return_value=("usb-host-lab",),
                ),
            ):
                result = apply_prune(cache)

            self.assertEqual(
                result.removed,
                (
                    f"logs/build/nokia/profiles/usb-host-lab/{build_runs[0].name}",
                    f"logs/check/profiles/usb-host-lab/{check_runs[0].name}",
                ),
            )
            self.assertFalse(check_runs[0].exists())
            self.assertFalse(build_runs[0].exists())
            self.assertTrue(check_runs[-1].exists())
            self.assertTrue(build_runs[-1].exists())

    def test_profile_log_cleanup_keeps_ten_valid_runs_and_preserves_malformed_entries(
        self,
    ) -> None:
        """Automatic retention removes only generated build runs in the selected profile slot."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            runs = [
                _cli_log(cache, "build", sequence, target="phone/profiles/host")
                for sequence in range(11)
            ]
            malformed = cache / "logs/build/phone/profiles/host/manual"
            malformed.mkdir()
            (malformed / "note").write_text("keep\n")

            removed = prune_module.discard_superseded_profile_logs(
                cache,
                "build",
                target="phone",
                profile="host",
            )

            self.assertEqual(removed, (f"logs/build/phone/profiles/host/{runs[0].name}",))
            self.assertFalse(runs[0].exists())
            self.assertTrue(runs[-1].exists())
            self.assertTrue(malformed.exists())

    def test_orphaned_profile_logs_are_all_disposable(self) -> None:
        """A deleted target/profile does not keep even its newest host-created log."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            check = _cli_log(cache, "check", 0, target="profiles/removed")
            build = _cli_log(cache, "build", 0, target="deleted/profiles/removed")

            with (
                mock.patch.object(prune_module, "discover_targets", return_value=("current",)),
                mock.patch.object(prune_module, "discover_profiles", return_value=()),
            ):
                result = apply_prune(cache)

            self.assertEqual(
                result.removed,
                (
                    "logs/build/deleted/profiles/removed",
                    "logs/check/profiles/removed",
                ),
            )
            self.assertFalse(check.exists())
            self.assertFalse(build.exists())

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
