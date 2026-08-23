# SPDX-License-Identifier: GPL-2.0-only
"""Tests for causal immutable workspace snapshots and their cache publication."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli import alpine_state, config
from fplinux_cli import workspace as workspace_module


class WorkspaceSnapshotTests(unittest.TestCase):
    """Keep cache materialization causally bound to an already read snapshot."""

    def _source(self, root: Path, *, contents: bytes = b"source", mode: int = 0o754) -> Path:
        source = root / "source"
        source.write_bytes(contents)
        source.chmod(mode)
        return source

    def test_target_snapshot_reads_path_bytes_and_mode_without_creating_cache(self) -> None:
        """A target recipe is available before any workspace directory is materialized."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self._source(root, contents=b"first", mode=0o751)
            with (
                mock.patch.object(workspace_module, "ROOT", root),
                mock.patch.object(
                    workspace_module,
                    "target_build_source_files",
                    return_value=[("nested/source", source)],
                ),
            ):
                snapshot = workspace_module.target_workspace_snapshot("demo")

            self.assertEqual(
                snapshot.files,
                (workspace_module.WorkspaceFile("nested/source", b"first", 0o751),),
            )
            self.assertEqual(len(snapshot.recipe), 64)
            self.assertFalse((root / ".cache").exists())

    def test_declared_target_inputs_are_causal_and_unrelated_files_are_not(self) -> None:
        """Declared package and source changes alter the recipe; unrelated files do not."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            def write(relative: str, contents: bytes = b"source\n") -> Path:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(contents)
                return path

            always = write("always.txt")
            rootfs_apkbuild = write("alpine/aports/package-a/APKBUILD")
            bundle_apkbuild = write("alpine/aports/package-b/APKBUILD")
            shared = write("shared/dependency.c")
            target_copy = write("targets/phone/kernel/copy.c")
            platform_patch = write("shared/platform.patch")
            host_tool = write("tools/loader.c")
            unrelated = write("unselected.txt")

            for relative in (
                "targets/phone/target.toml",
                "targets/phone/release/manifest.toml",
                "targets/phone/loader/assets.lock.toml",
                "targets/phone/kernel/defconfig",
                "targets/phone/bootstrap/main.c",
                "targets/phone/kernel/append.cfg",
                "platforms/demo/platform.toml",
                "shared/platform-copy.c",
                "shared/platform-append.cfg",
                "shared/bootstrap/main.c",
                "common/run.py",
                "platforms/demo/host/adapter.py",
            ):
                write(relative)

            target: dict[str, Any] = {
                "platform": "demo",
                "bundle": {"packages": ["package-b"]},
                "linux": {
                    "patches": [],
                    "copies": [{"source": "kernel/copy.c"}],
                    "appends": [{"source": "kernel/append.cfg"}],
                },
                "bootstrap": {"source": "bootstrap"},
            }
            platform: dict[str, Any] = {
                "bundle": {"packages": []},
                "linux": {
                    "patches": ["shared/platform.patch"],
                    "copies": [{"source": "shared/platform-copy.c"}],
                    "appends": [{"source": "shared/platform-append.cfg"}],
                },
                "bootstrap": {"shared_copies": [{"source": "shared/bootstrap"}]},
                "host": {"tools": [{"type": "cc-libusb", "source": "tools/loader.c"}]},
            }

            def shared_sources(package: str, root: Path) -> tuple[Path, ...]:
                del root
                return (shared,) if package in {"package-a", "package-b"} else ()

            with (
                mock.patch.object(workspace_module, "ROOT", root),
                mock.patch.object(config, "ROOT", root),
                mock.patch.object(workspace_module, "STAGED_BUILD_SOURCES", ("always.txt",)),
                mock.patch.object(workspace_module, "load_target", return_value=target),
                mock.patch.object(workspace_module, "load_platform", return_value=platform),
                mock.patch.object(
                    alpine_state,
                    "selected_packages",
                    return_value=("package-a",),
                ),
                mock.patch.object(
                    alpine_state,
                    "bundle_packages",
                    return_value=("package-b",),
                ),
                mock.patch.object(
                    alpine_state,
                    "shared_aport_sources",
                    side_effect=shared_sources,
                ),
            ):
                baseline = workspace_module.target_workspace_snapshot("phone").recipe
                for causal in (
                    always,
                    rootfs_apkbuild,
                    bundle_apkbuild,
                    shared,
                    target_copy,
                    platform_patch,
                    host_tool,
                ):
                    original = causal.read_bytes()
                    causal.write_bytes(original + b"changed\n")
                    self.assertNotEqual(
                        workspace_module.target_workspace_snapshot("phone").recipe,
                        baseline,
                        causal,
                    )
                    causal.write_bytes(original)

                unrelated.write_bytes(b"changed but unrelated\n")
                self.assertEqual(
                    workspace_module.target_workspace_snapshot("phone").recipe,
                    baseline,
                )

    def test_snapshot_recipe_includes_file_mode(self) -> None:
        """Changing only execute permissions changes the causal recipe."""
        with tempfile.TemporaryDirectory() as temporary:
            source = self._source(Path(temporary), mode=0o644)
            first = workspace_module.workspace_snapshot([("source", source)])
            source.chmod(0o755)
            second = workspace_module.workspace_snapshot([("source", source)])

            self.assertNotEqual(first.recipe, second.recipe)

    def test_snapshot_rejects_symlinked_input(self) -> None:
        """A snapshot cannot turn a linked source into a regular staged file."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self._source(root)
            linked = root / "linked"
            linked.symlink_to(source)

            with self.assertRaisesRegex(SystemExit, "workspace input must be a regular file"):
                workspace_module.workspace_snapshot([("linked", linked)])

    def test_materialization_uses_precomputed_bytes_after_checkout_changes(self) -> None:
        """Staging reads only the snapshot, so a later checkout edit cannot leak in."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self._source(root, contents=b"snapshot", mode=0o751)
            snapshot = workspace_module.workspace_snapshot([("bin/tool", source)])
            source.write_bytes(b"new-checkout")
            source.chmod(0o644)

            with mock.patch.object(workspace_module, "ROOT", root):
                staged = workspace_module.stage_workspace_snapshot(snapshot)

            output = staged / "bin/tool"
            self.assertEqual(output.read_bytes(), b"snapshot")
            self.assertEqual(output.stat().st_mode & 0o777, 0o751)

    def test_valid_exact_cache_is_a_hit_without_new_staging_directory(self) -> None:
        """An exact cache entry returns before tempfile creation."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            snapshot = workspace_module.workspace_snapshot([("source", self._source(root))])
            with mock.patch.object(workspace_module, "ROOT", root):
                expected = workspace_module.stage_workspace_snapshot(snapshot)
                with mock.patch(
                    "fplinux_cli.workspace.tempfile.mkdtemp",
                    side_effect=AssertionError("cache hit must not stage"),
                ):
                    actual = workspace_module.stage_workspace_snapshot(snapshot)

            self.assertEqual(actual, expected)

    def test_failed_materialization_removes_its_staging_directory(self) -> None:
        """An injected write failure removes the private staging directory."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            snapshot = workspace_module.workspace_snapshot([("source", self._source(root))])
            with (
                mock.patch.object(workspace_module, "ROOT", root),
                mock.patch.object(
                    workspace_module,
                    "_write_snapshot_file",
                    side_effect=OSError("write failed"),
                ),
                self.assertRaisesRegex(OSError, "write failed"),
            ):
                workspace_module.stage_workspace_snapshot(snapshot)

            self.assertEqual(list(root.rglob(".stage-*")), [])

    def test_source_policy_rejects_python_cache_before_staging(self) -> None:
        """Do not omit a forbidden generated artifact from the source snapshot."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / "module.pyc"
            cache.write_bytes(b"generated")
            with (
                mock.patch.object(workspace_module, "ROOT", root),
                mock.patch(
                    "fplinux_cli.workspace.subprocess.run",
                    return_value=subprocess.CompletedProcess(
                        ["git", "ls-files"], 0, b"module.pyc\0", b""
                    ),
                ),
                self.assertRaisesRegex(SystemExit, "generated Python cache"),
            ):
                workspace_module.quality_files(enforce_source_policy=True)
            with (
                mock.patch.object(workspace_module, "ROOT", root),
                mock.patch(
                    "fplinux_cli.workspace.subprocess.run",
                    return_value=subprocess.CompletedProcess(
                        ["git", "ls-files"], 0, b"module.pyc\0", b""
                    ),
                ),
            ):
                self.assertEqual(
                    workspace_module.quality_files(enforce_source_policy=False),
                    [],
                )

    def test_quality_inventory_uses_git_excludes(self) -> None:
        """Do not stage machine-local files omitted by Git's tracked inventory."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            source = root / "source.py"
            source.write_text("source\n", encoding="utf-8")
            local = root / "local-tools/settings.json"
            local.parent.mkdir(parents=True)
            local.write_text("{}\n", encoding="utf-8")
            (root / ".git/info/exclude").write_text("/local-tools/\n", encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", "--", "source.py"], check=True)

            with mock.patch.object(workspace_module, "ROOT", root):
                files = workspace_module.quality_files(enforce_source_policy=True)

            self.assertEqual(files, [("source.py", source)])


if __name__ == "__main__":
    unittest.main()
