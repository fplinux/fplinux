# SPDX-License-Identifier: GPL-2.0-only
"""Causal immutable source snapshots before cache materialization."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

import fplinux_cli.alpine.registration as alpine_registration
import fplinux_cli.workspace.build_inputs as workspace_inputs
import fplinux_cli.workspace.capture as workspace_capture
import fplinux_cli.workspace.quality_inputs as workspace_quality
import fplinux_cli.workspace.staging as workspace_staging
from fplinux_cli import common
from fplinux_cli.manifests.linux import discover_linux_targets

from tests.fixtures import linux_inputs
from tests.small.workspace.workspace_fixtures import (
    WorkspaceSourceFixture,
    empty_package_graph,
    workspace_root,
)


class WorkspaceSnapshotTests(WorkspaceSourceFixture):
    """Keep cache materialization causally bound to an already read snapshot."""

    def test_registration_changes_follow_selected_graph_and_materialized_bytes(self) -> None:
        """Unselected declarations keep the build recipe; selected dependency edges miss."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self._source(root)
            registration_name, registration_file = self._registration_source(root)
            for package in ("fplinux-selected", "fplinux-dependency", "fplinux-unrelated"):
                directory = root / "alpine/aports" / package
                directory.mkdir(parents=True)
                (directory / "APKBUILD").write_text(f"pkgname={package}\n")
            target = {
                "platform": "demo",
                "device_data": {"groups": {}},
                "rootfs": {
                    "base_packages": ["fplinux-selected"],
                    "packages": [],
                    "exclude_packages": [],
                },
                "bundle": {"packages": []},
            }
            platform: dict[str, Any] = {"rootfs": {"packages": []}, "bundle": {"packages": []}}
            dependencies: dict[str, tuple[str, ...]] = {}
            with (
                workspace_root(root),
                mock.patch.object(workspace_inputs, "load_target", return_value=target),
                mock.patch.object(workspace_inputs, "load_platform", return_value=platform),
                mock.patch.object(
                    workspace_inputs,
                    "shared_linux_source_files",
                    return_value=workspace_inputs.SharedLinuxSources(),
                ),
                mock.patch.object(
                    workspace_inputs,
                    "target_build_source_files",
                    return_value=[("source", source), (registration_name, registration_file)],
                ),
                mock.patch.object(alpine_registration, "COMMON_PACKAGES", ()),
                mock.patch.object(alpine_registration, "SUBPACKAGE_APORTS", {}),
                mock.patch.object(alpine_registration, "LOCAL_BUILD_DEPENDENCIES", dependencies),
                mock.patch.object(alpine_registration, "SHARED_APORT_SOURCES", {}),
            ):
                before = workspace_inputs.target_workspace_snapshot("demo")
                first_path = workspace_staging.stage_workspace_snapshot(before)
                self.assertEqual(workspace_staging.stage_workspace_snapshot(before), first_path)

                dependencies["fplinux-unrelated"] = ("fplinux-dependency",)
                registration_file.write_bytes(b"# unselected dependency declaration\n")
                unrelated = workspace_inputs.target_workspace_snapshot("demo")
                self.assertEqual(unrelated.recipe, before.recipe)
                self.assertNotEqual(
                    unrelated.materialization_recipe, before.materialization_recipe
                )
                second_path = workspace_staging.stage_workspace_snapshot(unrelated)
                self.assertNotEqual(second_path, first_path)
                self.assertEqual(
                    (second_path / registration_name).read_bytes(),
                    b"# unselected dependency declaration\n",
                )

                dependencies["fplinux-selected"] = ("fplinux-dependency",)
                registration_file.write_bytes(b"# selected dependency declaration\n")
                selected = workspace_inputs.target_workspace_snapshot("demo")
                self.assertNotEqual(selected.recipe, unrelated.recipe)

    def test_type_separates_identical_source_snapshots_and_survives_materialization(self) -> None:
        """Even identical fragment bytes cannot give two selected types one workspace identity."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self._source(root)
            with (
                workspace_root(root),
                empty_package_graph(),
                mock.patch.object(workspace_inputs, "load_platform", return_value={}),
                mock.patch.object(
                    workspace_inputs,
                    "shared_linux_source_files",
                    return_value=workspace_inputs.SharedLinuxSources(),
                ),
                mock.patch.object(
                    workspace_inputs,
                    "target_build_source_files",
                    return_value=[("source", source), self._registration_source(root)],
                ),
                mock.patch.object(
                    workspace_inputs,
                    "load_target",
                    return_value={"platform": "demo", "device_data": {"groups": {}}},
                ),
            ):
                release = workspace_inputs.target_workspace_snapshot("demo", build_type="release")
                debug = workspace_inputs.target_workspace_snapshot("demo", build_type="debug")
                self.assertEqual(release.files, debug.files)
                self.assertNotEqual(release.recipe, debug.recipe)
                release_path = workspace_staging.stage_workspace_snapshot(release)
                debug_path = workspace_staging.stage_workspace_snapshot(debug)
                self.assertNotEqual(release_path, debug_path)
                self.assertEqual((release_path / "source").read_bytes(), b"source")
                self.assertEqual((debug_path / "source").read_bytes(), b"source")
                self.assertEqual(workspace_staging.stage_workspace_snapshot(release), release_path)

    def test_target_snapshot_reads_path_bytes_and_mode_without_creating_cache(self) -> None:
        """A target recipe is available before any workspace directory is materialized."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self._source(root, contents=b"first", mode=0o751)
            with (
                workspace_root(root),
                empty_package_graph(),
                mock.patch.object(workspace_inputs, "load_platform", return_value={}),
                mock.patch.object(
                    workspace_inputs,
                    "shared_linux_source_files",
                    return_value=workspace_inputs.SharedLinuxSources(),
                ),
                mock.patch.object(
                    workspace_inputs,
                    "target_build_source_files",
                    return_value=[("nested/source", source), self._registration_source(root)],
                ),
                mock.patch.object(
                    workspace_inputs,
                    "load_target",
                    return_value={"platform": "demo", "device_data": {"groups": {}}},
                ),
            ):
                snapshot = workspace_inputs.target_workspace_snapshot("demo")

            self.assertEqual(
                snapshot.files,
                (workspace_capture.WorkspaceFile("nested/source", b"first", 0o751),),
            )
            self.assertEqual(len(snapshot.recipe), 64)
            self.assertFalse((root / ".cache").exists())

    def test_declared_target_inputs_are_causal_and_unrelated_files_are_not(self) -> None:
        """Declared package and source changes alter the recipe; unrelated files do not."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            self._registration_source(root)

            def write(relative: str, contents: bytes = b"source\n") -> Path:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(contents)
                return path

            always = write("always.txt")
            rootfs_apkbuild = write("alpine/aports/package-a/APKBUILD")
            bundle_apkbuild = write("alpine/aports/package-b/APKBUILD")
            library_apkbuild = write("alpine/aports/package-c/APKBUILD")
            shared = write("shared/dependency.c")
            target_copy = write("targets/phone/kernel/copy.c")
            platform_patch = write("shared/platform.patch")
            bootstrap_patch = write("patches/bootstrap.patch")
            host_tool = write("tools/loader.c")
            host_cli_source = write("lib/fplinux/fplinux-cli.c")
            host_cli_header = write("include/fplinux/fplinux-cli.h")
            host_input = write("tools/local-input.h")
            host_patch = write("tools/local.patch")
            loader_events = write("common/loader_events.py")
            write("alpine/ramroot-init.sh", b"#!/bin/sh\nexit 0\n")
            unrelated = write("unselected.txt")

            for relative in (
                "targets/phone/target.toml",
                "targets/phone/release/manifest.toml",
                "targets/phone/loader/assets.lock.toml",
                "targets/phone/kernel/config.fragment",
                "platforms/demo/kernel/defconfig",
                "platforms/demo/kernel/release.config",
                "profiles/default/profile.toml",
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
                "build_type": "release",
                "platform": "demo",
                "device_data": {"groups": {}},
                "bundle": {"packages": ["package-b"]},
                "linux": {
                    "root": {"kind": "initramfs"},
                    "config_fragment": "kernel/config.fragment",
                    "patches": [],
                    "copies": [{"source": "kernel/copy.c"}],
                    "appends": [{"source": "kernel/append.cfg"}],
                },
                "bootstrap": {"source": "bootstrap"},
                "uboot": {"kind": "none"},
                "fit": {"kind": "none"},
                "image": {"kind": "none"},
                "runtime": {},
            }
            platform: dict[str, Any] = {
                "bundle": {"packages": []},
                "linux": {
                    "defconfig": "platforms/demo/kernel/defconfig",
                    "build_types": {"release": "platforms/demo/kernel/release.config"},
                    "patches": ["shared/platform.patch"],
                    "copies": [{"source": "shared/platform-copy.c"}],
                    "appends": [{"source": "shared/platform-append.cfg"}],
                },
                "bootstrap": {
                    "shared_copies": [{"source": "shared/bootstrap"}],
                    "patches": ["patches/bootstrap.patch"],
                },
                "host": {
                    "tools": [
                        {"type": "cc-libusb", "source": "tools/loader.c"},
                        {
                            "type": "make-archive",
                            "copies": [
                                {
                                    "source": "tools/local-input.h",
                                    "destination": "local-input.h",
                                }
                            ],
                            "patches": ["tools/local.patch"],
                        },
                    ]
                },
            }

            def shared_sources(package: str, root: Path) -> tuple[Path, ...]:
                del root
                return (shared,) if package in {"package-a", "package-b"} else ()

            with (
                workspace_root(root),
                mock.patch.object(
                    workspace_inputs,
                    "shared_linux_source_files",
                    return_value=workspace_inputs.SharedLinuxSources(),
                ),
                mock.patch.object(common, "ROOT", root),
                mock.patch.object(workspace_inputs, "STAGED_BUILD_SOURCES", ("always.txt",)),
                mock.patch.object(workspace_inputs, "selected_build_sources", return_value=()),
                mock.patch.object(workspace_inputs, "load_target", return_value=target),
                mock.patch.object(workspace_inputs, "load_platform", return_value=platform),
                mock.patch.object(
                    workspace_inputs,
                    "selected_packages",
                    return_value=("package-a",),
                ),
                mock.patch.object(
                    workspace_inputs,
                    "bundle_packages",
                    return_value=("package-b-extra",),
                ),
                mock.patch.object(
                    alpine_registration,
                    "SUBPACKAGE_APORTS",
                    {"package-b-extra": "package-b"},
                ),
                mock.patch.object(
                    alpine_registration,
                    "LOCAL_BUILD_DEPENDENCIES",
                    {"package-a": ("package-c",)},
                ),
                mock.patch.object(
                    workspace_inputs,
                    "shared_aport_sources",
                    side_effect=shared_sources,
                ),
            ):
                baseline = workspace_inputs.target_workspace_snapshot("phone").recipe
                for causal in (
                    always,
                    rootfs_apkbuild,
                    bundle_apkbuild,
                    library_apkbuild,
                    shared,
                    target_copy,
                    platform_patch,
                    bootstrap_patch,
                    host_tool,
                    host_cli_source,
                    host_cli_header,
                    host_input,
                    host_patch,
                    loader_events,
                ):
                    original = causal.read_bytes()
                    causal.write_bytes(original + b"changed\n")
                    self.assertNotEqual(
                        workspace_inputs.target_workspace_snapshot("phone").recipe,
                        baseline,
                        causal,
                    )
                    causal.write_bytes(original)

                unrelated.write_bytes(b"changed but unrelated\n")
                self.assertEqual(
                    workspace_inputs.target_workspace_snapshot("phone").recipe,
                    baseline,
                )

    def test_snapshot_recipe_includes_file_mode(self) -> None:
        """Changing only execute permissions changes the causal recipe."""
        with tempfile.TemporaryDirectory() as temporary:
            source = self._source(Path(temporary), mode=0o644)
            first = workspace_capture.workspace_snapshot([("source", source)])
            source.chmod(0o755)
            second = workspace_capture.workspace_snapshot([("source", source)])

            self.assertNotEqual(first.recipe, second.recipe)

    def test_peer_linux_edits_update_staging_without_invalidating_selected_recipe(self) -> None:
        """Peer C/DTS changes stay auxiliary while a shared driver remains causal."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            linux_inputs.write(root, "sources.lock.toml", f'[linux]\nsha256 = "{"1" * 64}"\n')
            platform = linux_inputs.platform(root, "demo", source_lock="linux", arch="arm")
            linux_inputs.platform(root, "other", source_lock="linux", arch="riscv")
            selected = linux_inputs.target(root, "phone-a", platform_name="demo")
            peer_manifest = linux_inputs.target(
                root, "phone-b", platform_name="other", arch="riscv"
            )
            peer_manifest.write_text(
                peer_manifest.read_text(encoding="utf-8")
                .replace("patches = []", 'patches = ["linux/shared.patch"]', 1)
                .replace(
                    "appends = []",
                    'appends = [{source = "linux/shared.Kconfig", '
                    'destination = "drivers/Kconfig"}]',
                ),
                encoding="utf-8",
            )
            peer_patch = linux_inputs.write(
                root,
                "targets/phone-b/linux/shared.patch",
                "--- a/drivers/shared.c\n+++ b/drivers/shared.c\n@@ -1 +1 @@\n-old\n+new\n",
            )
            peer_append = linux_inputs.write(
                root, "targets/phone-b/linux/shared.Kconfig", "config BOARD_B\n\tbool\n"
            )
            common_driver = root / "platforms/demo/common.c"
            causal_sources = [
                (path.relative_to(root).as_posix(), path)
                for path in (
                    root / "sources.lock.toml",
                    platform,
                    selected,
                    common_driver,
                    root / "targets/phone-a/linux/board.c",
                    root / "targets/phone-a/linux/board.dts",
                )
            ]
            causal_sources.append(self._registration_source(root))
            target_config = {"platform": "demo", "device_data": {"groups": {}}}
            with (
                workspace_root(root),
                empty_package_graph(),
                mock.patch.object(workspace_inputs, "load_target", return_value=target_config),
                mock.patch.object(
                    workspace_inputs, "load_platform", return_value=common.load_toml(platform)
                ),
                mock.patch.object(
                    workspace_inputs, "target_build_source_files", return_value=causal_sources
                ),
            ):
                before = workspace_inputs.target_workspace_snapshot("phone-a")
                before_path = workspace_staging.stage_workspace_snapshot(before)
                peer_driver = root / "targets/phone-b/linux/board.c"
                peer_dts = root / "targets/phone-b/linux/board.dts"
                peer_driver.write_bytes(b"changed peer driver\n")
                peer_driver.chmod(0o640)
                peer_dts.write_bytes(b"changed peer device tree\n")
                changed = workspace_inputs.target_workspace_snapshot("phone-a")
                changed_path = workspace_staging.stage_workspace_snapshot(changed)
                self.assertEqual(before.recipe, changed.recipe)
                self.assertNotEqual(before_path, changed_path)
                self.assertEqual(
                    (before_path / "targets/phone-b/linux/board.c").read_bytes(),
                    b"phone-b driver\n",
                )
                staged_peer = changed_path / "targets/phone-b/linux/board.c"
                self.assertEqual(staged_peer.read_bytes(), b"changed peer driver\n")
                self.assertEqual(staged_peer.stat().st_mode & 0o777, 0o640)
                self.assertEqual(
                    (changed_path / "targets/phone-b/linux/board.dts").read_bytes(),
                    b"changed peer device tree\n",
                )
                staged_targets = discover_linux_targets(
                    changed_path, {"linux": {"sha256": "1" * 64}}, "1" * 64
                )
                self.assertEqual(
                    [target.name for target in staged_targets], ["phone-a", "phone-b"]
                )

                for relative in ("bootstrap/main.c", "firmware/private.bin", "rootfs/config"):
                    linux_inputs.write(root, f"targets/phone-b/{relative}", "unrelated\n")
                unrelated = workspace_inputs.target_workspace_snapshot("phone-a")
                self.assertEqual(changed.recipe, unrelated.recipe)
                self.assertEqual(changed.materialization_recipe, unrelated.materialization_recipe)
                with peer_manifest.open("a", encoding="utf-8") as output:
                    output.write(
                        '\n[rootfs]\npackages = ["unrelated"]\n[bootstrap]\nsource = "missing"\n'
                    )
                self.assertEqual(
                    workspace_inputs.target_workspace_snapshot("phone-a").recipe, changed.recipe
                )

                common_driver.write_bytes(b"changed shared driver\n")
                shared_change = workspace_inputs.target_workspace_snapshot("phone-a")
                self.assertNotEqual(changed.recipe, shared_change.recipe)
                common_driver.write_bytes(b"common driver\n")
                for shared in (peer_patch, peer_append, root / "platforms/other/common.c"):
                    original = shared.read_bytes()
                    shared.write_bytes(original + b"changed\n")
                    self.assertNotEqual(
                        workspace_inputs.target_workspace_snapshot("phone-a").recipe,
                        changed.recipe,
                        shared,
                    )
                    shared.write_bytes(original)
                peer_manifest.write_text(
                    peer_manifest.read_text(encoding="utf-8").replace(
                        'destination = "drivers/Kconfig"', 'destination = "arch/arm/Kconfig"'
                    ),
                    encoding="utf-8",
                )
                redirected = workspace_inputs.target_workspace_snapshot("phone-a")
                self.assertNotEqual(redirected.recipe, changed.recipe)
                peer_manifest.write_text(
                    peer_manifest.read_text(encoding="utf-8").replace(
                        'destination = "drivers/other/phone-b.c"',
                        'destination = "drivers/shared.c"',
                    ),
                    encoding="utf-8",
                )
                self.assertNotEqual(
                    workspace_inputs.target_workspace_snapshot("phone-a").recipe,
                    redirected.recipe,
                )
                workspace_staging.discard_staged_workspace_snapshot(changed, changed_path)
                self.assertFalse(changed_path.exists())
                self.assertTrue(before_path.is_dir())

    def test_bootstrap_edit_changes_ram_source_recipe_but_not_microsd(self) -> None:
        """An edit consumed only by RAM boot leaves the SD source recipe reusable."""
        files = dict(workspace_inputs.target_build_source_files("nokia-ta1618"))
        files.update(workspace_inputs.target_build_source_files("nokia-ta1618", "microsd-uboot"))
        snapshot = workspace_capture.workspace_snapshot(sorted(files.items()))
        with tempfile.TemporaryDirectory() as temporary:
            with workspace_root(Path(temporary)):
                source = workspace_staging.stage_workspace_snapshot(snapshot)
            bootstrap = source / "alpine/ramroot-init.sh"
            bootstrap.write_bytes(b"#!/bin/sh\nexit 0\n")
            with (
                workspace_root(source),
                mock.patch.object(common, "ROOT", source),
            ):
                before_ram = workspace_capture.workspace_snapshot(
                    workspace_inputs.target_build_source_files("nokia-ta1618")
                ).recipe
                before_sd = workspace_capture.workspace_snapshot(
                    workspace_inputs.target_build_source_files("nokia-ta1618", "microsd-uboot")
                ).recipe
                bootstrap.write_bytes(b"#!/bin/sh\nexit 1\n")
                after_ram = workspace_capture.workspace_snapshot(
                    workspace_inputs.target_build_source_files("nokia-ta1618")
                ).recipe
                after_sd = workspace_capture.workspace_snapshot(
                    workspace_inputs.target_build_source_files("nokia-ta1618", "microsd-uboot")
                ).recipe
            self.assertNotEqual(before_ram, after_ram)
            self.assertEqual(before_sd, after_sd)

    def test_snapshot_rejects_symlinked_input(self) -> None:
        """A snapshot cannot turn a linked source into a regular staged file."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self._source(root)
            linked = root / "linked"
            linked.symlink_to(source)

            with self.assertRaisesRegex(SystemExit, "workspace input must be a regular file"):
                workspace_capture.workspace_snapshot([("linked", linked)])

    def test_source_policy_rejects_python_cache_before_staging(self) -> None:
        """Do not omit a forbidden generated artifact from the source snapshot."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / "module.pyc"
            cache.write_bytes(b"generated")
            with (
                workspace_root(root),
                mock.patch(
                    "fplinux_cli.workspace.quality_inputs.subprocess.run",
                    return_value=subprocess.CompletedProcess(
                        ["git", "ls-files"], 0, b"module.pyc\0", b""
                    ),
                ),
                self.assertRaisesRegex(SystemExit, "generated Python cache"),
            ):
                workspace_quality.quality_files(enforce_source_policy=True)
            with (
                workspace_root(root),
                mock.patch(
                    "fplinux_cli.workspace.quality_inputs.subprocess.run",
                    return_value=subprocess.CompletedProcess(
                        ["git", "ls-files"], 0, b"module.pyc\0", b""
                    ),
                ),
            ):
                self.assertEqual(
                    workspace_quality.quality_files(enforce_source_policy=False),
                    [],
                )

    def test_quality_inventory_reports_a_git_timeout(self) -> None:
        """A stuck Git inventory fails at its named boundary without staging files."""
        with (
            mock.patch(
                "fplinux_cli.workspace.quality_inputs.subprocess.run",
                side_effect=subprocess.TimeoutExpired(["git", "ls-files"], 60),
            ),
            self.assertRaisesRegex(SystemExit, "Git source inventory timed out"),
        ):
            workspace_quality.quality_files(enforce_source_policy=False)


if __name__ == "__main__":
    unittest.main()
