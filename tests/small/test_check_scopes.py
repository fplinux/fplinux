# SPDX-License-Identifier: GPL-2.0-only
"""Tests for inner check-scope closure and host orchestration semantics."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import Literal, Self
from unittest import mock

from fplinux_cli import container
from fplinux_cli.checkreceipts import publish_success_receipt, receipt_matches
from fplinux_cli.container import (
    check_scope_closure_digest,
    check_scope_receipt_recipe,
    resolve_check_scopes,
)
from fplinux_cli.workspace import WorkspaceFile, WorkspaceSnapshot


class CheckScopeTests(unittest.TestCase):
    """Keep scope selection stable and independent of argument order."""

    def test_selection_is_deduplicated_in_canonical_order(self) -> None:
        """Deduplicate selections and ignore their command-line order."""
        self.assertEqual(
            resolve_check_scopes(["kernel", "python", "kernel", "repository"]),
            ("repository", "python", "kernel"),
        )

    def test_scope_receipt_misses_after_orchestration_or_oci_identity_changes(self) -> None:
        """Do not reuse one scope across checker or immutable image changes."""
        closure = "a" * 64
        first_orchestration = "b" * 64
        image = "sha256:" + "c" * 64
        with mock.patch.object(
            container,
            "check_orchestration_recipe_digest",
            return_value=first_orchestration,
        ):
            first = check_scope_receipt_recipe("python", closure, image_identity=image)
            image_changed = check_scope_receipt_recipe(
                "python", closure, image_identity="sha256:" + "d" * 64
            )
        with mock.patch.object(
            container,
            "check_orchestration_recipe_digest",
            return_value="e" * 64,
        ):
            orchestration_changed = check_scope_receipt_recipe(
                "python", closure, image_identity=image
            )
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            publish_success_receipt(cache, first)
            self.assertTrue(receipt_matches(cache, first))
            self.assertFalse(receipt_matches(cache, image_changed))
            self.assertFalse(receipt_matches(cache, orchestration_changed))

    def test_named_kernel_profile_has_its_own_receipt_identity(self) -> None:
        """Profile selection keeps default and named cache slots distinct."""
        default = check_scope_receipt_recipe(
            "kernel",
            "a" * 64,
            image_identity="sha256:" + "b" * 64,
            orchestration_recipe="c" * 64,
        )
        profile = check_scope_receipt_recipe(
            "kernel",
            "a" * 64,
            image_identity="sha256:" + "b" * 64,
            orchestration_recipe="c" * 64,
            profile="usb-host-lab",
        )

        self.assertIsNone(default.profile)
        self.assertEqual(profile.profile, "usb-host-lab")
        self.assertNotEqual(default.payload(), profile.payload())

    def test_readme_does_not_invalidate_c_or_kernel_scope(self) -> None:
        """Keep unrelated documentation outside the two expensive closures."""
        common = (
            WorkspaceFile("README.md", b"first", 0o644),
            WorkspaceFile("alpine/aports/demo-consumer/app.c", b"int app;\n", 0o644),
            WorkspaceFile("targets/phone/kernel/board.c", b"int board;\n", 0o644),
            WorkspaceFile("scripts/fplinux_cli/kernelcheck.py", b"checker\n", 0o644),
            WorkspaceFile(
                "targets/phone/target.toml",
                b'platform = "phone"\n'
                b"[linux]\n"
                b'copies = [{ source = "kernel/board.c", destination = "board.c" }]\n',
                0o644,
            ),
            WorkspaceFile(
                "platforms/phone/platform.toml",
                b"[linux]\npatches = []\ncopies = []\nappends = []\n",
                0o644,
            ),
        )
        changed = (WorkspaceFile("README.md", b"second", 0o644), *common[1:])
        first = WorkspaceSnapshot(common, "a" * 64)
        second = WorkspaceSnapshot(changed, "b" * 64)
        self.assertEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", second),
        )
        self.assertEqual(
            check_scope_closure_digest("kernel", first),
            check_scope_closure_digest("kernel", second),
        )
        self.assertNotEqual(
            check_scope_closure_digest("source", first),
            check_scope_closure_digest("source", second),
        )

        kernel_changed = WorkspaceSnapshot(
            (
                common[0],
                common[1],
                WorkspaceFile("targets/phone/kernel/board.c", b"int changed;\n", 0o644),
                common[3],
                common[4],
                common[5],
            ),
            "c" * 64,
        )
        self.assertEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", kernel_changed),
        )
        self.assertNotEqual(
            check_scope_closure_digest("kernel", first),
            check_scope_closure_digest("kernel", kernel_changed),
        )

    def test_c_harness_change_updates_the_c_closure(self) -> None:
        """Treat a host C harness as a causal input to the C scope."""
        first = WorkspaceSnapshot(
            (
                WorkspaceFile(
                    "tests/host_tool/harness.c",
                    b"int main(void) { return 0; }\n",
                    0o644,
                ),
            ),
            "a" * 64,
        )
        second = WorkspaceSnapshot(
            (
                WorkspaceFile(
                    "tests/host_tool/harness.c",
                    b"int main(void) { return 1; }\n",
                    0o644,
                ),
            ),
            "b" * 64,
        )

        self.assertNotEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", second),
        )

    def test_kernel_scope_tracks_manifest_sources_without_whole_bootstrap(self) -> None:
        """Track every projected Linux input but ignore bootstrap-only sources."""
        target_manifest = (
            b'platform = "demo"\n'
            b"[linux]\n"
            b'patches = ["kernel/target.patch"]\n'
            b"copies = [\n"
            b'  { source = "kernel/target-copy.c", destination = "target-copy.c" },\n'
            b'  { source = "bootstrap/referenced.h", destination = "referenced.h" },\n'
            b"]\n"
            b"appends = [\n"
            b'  { source = "kernel/target-append", destination = "Makefile" },\n'
            b"]\n"
        )
        platform_manifest = (
            b"[linux]\n"
            b'patches = ["platforms/demo/kernel/platform.patch"]\n'
            b"copies = [\n"
            b'  { source = "platforms/demo/kernel/platform-copy.c", '
            b'destination = "platform-copy.c" },\n'
            b"]\n"
            b"appends = [\n"
            b'  { source = "platforms/demo/kernel/platform-append", destination = "Makefile" },\n'
            b"]\n"
        )
        files = (
            WorkspaceFile("scripts/fplinux_cli/kernelcheck.py", b"checker\n", 0o644),
            WorkspaceFile("targets/demo/target.toml", target_manifest, 0o644),
            WorkspaceFile("platforms/demo/platform.toml", platform_manifest, 0o644),
            WorkspaceFile("targets/demo/kernel/defconfig", b"CONFIG_DEMO=y\n", 0o644),
            WorkspaceFile("targets/demo/kernel/target.patch", b"target patch\n", 0o644),
            WorkspaceFile("targets/demo/kernel/target-copy.c", b"int target;\n", 0o644),
            WorkspaceFile("targets/demo/bootstrap/referenced.h", b"#define DEMO 1\n", 0o644),
            WorkspaceFile("targets/demo/kernel/target-append", b"obj-y += demo.o\n", 0o644),
            WorkspaceFile("platforms/demo/kernel/platform.patch", b"platform patch\n", 0o644),
            WorkspaceFile("platforms/demo/kernel/platform-copy.c", b"int platform;\n", 0o644),
            WorkspaceFile(
                "platforms/demo/kernel/platform-append", b"obj-y += platform.o\n", 0o644
            ),
            WorkspaceFile("targets/demo/bootstrap/main.c", b"int main;\n", 0o644),
        )
        first = WorkspaceSnapshot(files, "a" * 64)
        header_changed = WorkspaceSnapshot(
            (
                *files[:6],
                WorkspaceFile(files[6].path, b"#define DEMO 2\n", 0o644),
                *files[7:],
            ),
            "b" * 64,
        )
        bootstrap_changed = WorkspaceSnapshot(
            (
                *files[:-1],
                WorkspaceFile(files[-1].path, b"int changed;\n", 0o644),
            ),
            "c" * 64,
        )
        first_digest = check_scope_closure_digest("kernel", first)
        header_digest = check_scope_closure_digest("kernel", header_changed)
        self.assertNotEqual(first_digest, header_digest)
        self.assertEqual(
            first_digest,
            check_scope_closure_digest("kernel", bootstrap_changed),
        )
        first_recipe = check_scope_receipt_recipe(
            "kernel",
            first_digest,
            image_identity="sha256:" + "c" * 64,
            orchestration_recipe="d" * 64,
        )
        header_recipe = check_scope_receipt_recipe(
            "kernel",
            header_digest,
            image_identity="sha256:" + "c" * 64,
            orchestration_recipe="d" * 64,
        )
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            publish_success_receipt(cache, first_recipe)
            self.assertFalse(receipt_matches(cache, header_recipe))

    def test_named_kernel_profile_tracks_only_its_profile_manifest_and_sources(self) -> None:
        """A narrow profile check does not reuse another profile's kernel receipt."""
        target = WorkspaceFile(
            "targets/phone/target.toml",
            b'platform = "demo"\n[linux]\npatches = []\ncopies = []\nappends = []\n',
            0o644,
        )
        platform = WorkspaceFile(
            "platforms/demo/platform.toml",
            b"[linux]\npatches = []\ncopies = []\nappends = []\n",
            0o644,
        )
        host_manifest = WorkspaceFile(
            "targets/phone/profiles/host/profile.toml",
            b"[linux]\npatches = []\n"
            b'copies = [{ source = "host.c", destination = "host.c" }]\n'
            b"appends = []\n",
            0o644,
        )
        other_manifest = WorkspaceFile(
            "targets/phone/profiles/diagnostic/profile.toml",
            b"[linux]\npatches = []\n"
            b'copies = [{ source = "diagnostic.c", destination = "diagnostic.c" }]\n'
            b"appends = []\n",
            0o644,
        )
        files = (
            WorkspaceFile("scripts/fplinux_cli/kernelcheck.py", b"checker\n", 0o644),
            target,
            platform,
            host_manifest,
            other_manifest,
            WorkspaceFile("targets/phone/profiles/host/host.c", b"int host;\n", 0o644),
            WorkspaceFile(
                "targets/phone/profiles/diagnostic/diagnostic.c", b"int diagnostic;\n", 0o644
            ),
        )
        host_before = WorkspaceSnapshot(files, "a" * 64)
        other_changed = WorkspaceSnapshot(
            (*files[:-1], WorkspaceFile(files[-1].path, b"int changed;\n", 0o644)),
            "b" * 64,
        )
        host_changed = WorkspaceSnapshot(
            (
                *files[:5],
                WorkspaceFile(files[5].path, b"int changed;\n", 0o644),
                files[6],
            ),
            "c" * 64,
        )
        host_manifest_changed = WorkspaceSnapshot(
            (
                *files[:3],
                WorkspaceFile(files[3].path, b"not even TOML", 0o644),
                *files[4:],
            ),
            "d" * 64,
        )

        digest = check_scope_closure_digest("kernel", host_before, profile="host")
        self.assertEqual(
            digest,
            check_scope_closure_digest("kernel", other_changed, profile="host"),
        )
        self.assertNotEqual(
            digest,
            check_scope_closure_digest("kernel", host_changed, profile="host"),
        )
        self.assertEqual(
            check_scope_closure_digest("kernel", host_before),
            check_scope_closure_digest("kernel", host_changed),
        )
        self.assertEqual(
            check_scope_closure_digest("kernel", host_before),
            check_scope_closure_digest("kernel", host_manifest_changed),
        )

    def test_editorconfig_change_updates_metadata_closure(self) -> None:
        """Track Prettier's repository EditorConfig as metadata input."""
        first = WorkspaceSnapshot(
            (
                WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
                WorkspaceFile(".editorconfig", b"indent_size = 4\n", 0o644),
            ),
            "a" * 64,
        )
        second = WorkspaceSnapshot(
            (
                first.files[0],
                WorkspaceFile(".editorconfig", b"indent_size = 2\n", 0o644),
            ),
            "b" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("metadata", first),
            check_scope_closure_digest("metadata", second),
        )

    def test_executable_prettier_config_includes_local_helpers(self) -> None:
        """Imported local helpers are causal inputs to an executable Prettier config."""
        common = (
            WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
            WorkspaceFile("prettier.config.mjs", b'import "./helper.mjs";\n', 0o644),
        )
        first = WorkspaceSnapshot(
            (*common, WorkspaceFile("helper.mjs", b"first\n", 0o644)),
            "a" * 64,
        )
        second = WorkspaceSnapshot(
            (*common, WorkspaceFile("helper.mjs", b"second\n", 0o644)),
            "b" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("metadata", first),
            check_scope_closure_digest("metadata", second),
        )

    def test_c_scope_tracks_manifest_source_bootstrap_and_quoted_header(self) -> None:
        """Mirror dynamic userspace C discovery without including orphan kernel C."""
        manifest = (
            b"[host]\n"
            b'runtime_tools = { console = "tool" }\n'
            b"[[host.tools]]\n"
            b'type = "cc-libusb"\n'
            b'name = "tool"\n'
            b'source = "platforms/demo/kernel/tool.c"\n'
            b"self_test = false\n"
        )
        common = (
            WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
            WorkspaceFile("platforms/demo/platform.toml", manifest, 0o644),
            WorkspaceFile(
                "platforms/demo/kernel/tool.c",
                b'#include "tool.h"\nint tool;\n',
                0o644,
            ),
            WorkspaceFile("platforms/demo/kernel/tool.h", b"#define TOOL 1\n", 0o644),
            WorkspaceFile("targets/demo/kernel/orphan.c", b"int orphan;\n", 0o644),
            WorkspaceFile("targets/demo/kernel/bootstrap/start.c", b"int start;\n", 0o644),
        )
        first = WorkspaceSnapshot(common, "a" * 64)
        header_changed = WorkspaceSnapshot(
            (*common[:3], WorkspaceFile(common[3].path, b"#define TOOL 2\n", 0o644), *common[4:]),
            "b" * 64,
        )
        orphan_changed = WorkspaceSnapshot(
            (*common[:4], WorkspaceFile(common[4].path, b"int changed;\n", 0o644), common[5]),
            "c" * 64,
        )
        bootstrap_changed = WorkspaceSnapshot(
            (*common[:5], WorkspaceFile(common[5].path, b"int changed;\n", 0o644)),
            "d" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", header_changed),
        )
        self.assertEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", orphan_changed),
        )
        self.assertNotEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", bootstrap_changed),
        )

    def test_aport_c_and_header_invalidate_c_scope_without_runtime_selection(self) -> None:
        """Track every C/H input from an aport that may not be in the rootfs."""
        first = WorkspaceSnapshot(
            (
                WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
                WorkspaceFile(
                    "alpine/aports/local-only/local-only.c", b"int local_only;\n", 0o644
                ),
                WorkspaceFile(
                    "alpine/aports/local-only/local-only.h", b"#define LOCAL_ONLY 1\n", 0o644
                ),
            ),
            "a" * 64,
        )
        source_changed = WorkspaceSnapshot(
            (
                first.files[0],
                WorkspaceFile("alpine/aports/local-only/local-only.c", b"int changed;\n", 0o644),
                first.files[2],
            ),
            "b" * 64,
        )
        header_changed = WorkspaceSnapshot(
            (
                first.files[0],
                first.files[1],
                WorkspaceFile(
                    "alpine/aports/local-only/local-only.h", b"#define LOCAL_ONLY 2\n", 0o644
                ),
            ),
            "c" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", source_changed),
        )
        self.assertNotEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", header_changed),
        )

    def test_alpine_scope_tracks_each_present_apkbuild(self) -> None:
        """Track every aport and both package-selection manifest layers."""
        first = WorkspaceSnapshot(
            (
                WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
                WorkspaceFile("alpine.lock.toml", b"lock\n", 0o644),
                WorkspaceFile("alpine/abuild.conf", b"abuild\n", 0o644),
                WorkspaceFile("alpine/aports/local-only/APKBUILD", b"first\n", 0o644),
                WorkspaceFile("platforms/demo/platform.toml", b"platform packages\n", 0o644),
                WorkspaceFile("targets/demo/target.toml", b"target packages\n", 0o644),
            ),
            "a" * 64,
        )
        changed = WorkspaceSnapshot(
            (
                *first.files[:3],
                WorkspaceFile(first.files[3].path, b"second\n", 0o644),
                *first.files[4:],
            ),
            "b" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("alpine", first),
            check_scope_closure_digest("alpine", changed),
        )
        for index in (4, 5):
            files = list(first.files)
            files[index] = WorkspaceFile(files[index].path, b"changed packages\n", 0o644)
            with self.subTest(path=files[index].path):
                self.assertNotEqual(
                    check_scope_closure_digest("alpine", first),
                    check_scope_closure_digest(
                        "alpine", WorkspaceSnapshot(tuple(files), "c" * 64)
                    ),
                )

    def test_shell_scope_tracks_extensionless_and_openrc_sources(self) -> None:
        """Track shell sources matching the checker, including OpenRC init scripts."""
        base = WorkspaceSnapshot(
            (
                WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
                WorkspaceFile("tool", b"  #!/bin/sh  \necho ok\n", 0o755),
                WorkspaceFile("service.initd", b"#!/sbin/openrc-run\ncommand=/bin/true\n", 0o755),
                WorkspaceFile("helper.inc", b"first\n", 0o644),
            ),
            "a" * 64,
        )
        changed_tool = WorkspaceSnapshot(
            (
                base.files[0],
                WorkspaceFile("tool", b"  #!/bin/sh  \necho changed\n", 0o755),
                base.files[2],
            ),
            "b" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("shell", base),
            check_scope_closure_digest("shell", changed_tool),
        )
        changed_initd = WorkspaceSnapshot(
            (
                base.files[0],
                base.files[1],
                WorkspaceFile("service.initd", b"#!/sbin/openrc-run\ncommand=/bin/false\n", 0o755),
                base.files[3],
            ),
            "c" * 64,
        )
        image_identity = "sha256:" + "e" * 64
        first_recipe = check_scope_receipt_recipe(
            "shell",
            check_scope_closure_digest("shell", base),
            image_identity=image_identity,
            orchestration_recipe="f" * 64,
        )
        initd_recipe = check_scope_receipt_recipe(
            "shell",
            check_scope_closure_digest("shell", changed_initd),
            image_identity=image_identity,
            orchestration_recipe="f" * 64,
        )
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            publish_success_receipt(cache, first_recipe)
            self.assertTrue(receipt_matches(cache, first_recipe))
            self.assertFalse(receipt_matches(cache, initd_recipe))
        external = WorkspaceSnapshot(
            (*base.files, WorkspaceFile(".shellcheckrc", b"external-sources=true\n", 0o644)),
            "g" * 64,
        )
        external_changed = WorkspaceSnapshot(
            (
                external.files[0],
                external.files[1],
                external.files[2],
                WorkspaceFile("helper.inc", b"second\n", 0o644),
                external.files[4],
            ),
            "h" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("shell", external),
            check_scope_closure_digest("shell", external_changed),
        )


class RepositoryFastPathTests(unittest.TestCase):
    """Keep the repository-only check completely on the host."""

    def test_repository_check_returns_before_podman_or_workspace(self) -> None:
        """Return after the host check without requiring a container or snapshot."""
        reporter = mock.Mock()
        with (
            mock.patch("fplinux_cli.output.RunReporter.create", return_value=reporter),
            mock.patch.object(container, "check_git_diff") as git_diff,
            mock.patch.object(
                container,
                "require_podman",
                side_effect=AssertionError("repository check must not require Podman"),
            ),
            mock.patch.object(
                container,
                "quality_workspace_snapshot",
                side_effect=AssertionError("repository check must not snapshot a workspace"),
            ),
        ):
            container.check(["repository"])
        git_diff.assert_called_once_with(reporter)
        reporter.finish.assert_called_once_with()


class _RecordingStage:
    """Collect fake container argv without starting a subprocess."""

    def __init__(self, commands: list[list[str]]) -> None:
        self.commands = commands

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *_args: object) -> Literal[False]:
        return False

    def run(self, command: list[str], **_kwargs: object) -> None:
        self.commands.append(command)


class _FailingStage(_RecordingStage):
    """Fail the first attempted checker command."""

    def run(self, command: list[str], **_kwargs: object) -> None:
        self.commands.append(command)
        message = "scope failed"
        raise RuntimeError(message)


class _RecordingReporter:
    """Supply deterministic log plumbing to fake check stages."""

    def __init__(self, root: Path, commands: list[list[str]]) -> None:
        self.root = root
        self.commands = commands

    def stage(self, *_args: object, **_kwargs: object) -> _RecordingStage:
        return _RecordingStage(self.commands)

    def container_environment(self, mounted_root: str) -> dict[str, str]:
        return {
            "FPLINUX_LOG_ROOT": mounted_root,
            "FPLINUX_LOG_DISPLAY_ROOT": ".cache/logs/test",
        }

    def finish(self) -> None:
        return None


class _FailingReporter(_RecordingReporter):
    def stage(self, *_args: object, **_kwargs: object) -> _RecordingStage:
        return _FailingStage(self.commands)


class MockedCheckReceiptOrchestrationTests(unittest.TestCase):
    """Exercise real receipt state with mocked OCI and checker boundaries."""

    @staticmethod
    def _snapshot(*, c_source: bytes = b"int app;\n") -> WorkspaceSnapshot:
        files = (
            WorkspaceFile("README.md", b"documentation\n", 0o644),
            WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
            WorkspaceFile(
                "alpine/aports/demo-consumer/app.c",
                c_source,
                0o644,
            ),
        )
        return WorkspaceSnapshot(files, "a" * 64)

    def _run(  # noqa: PLR0913
        self,
        root: Path,
        workspace: Path,
        snapshot: WorkspaceSnapshot,
        scopes: list[str],
        commands: list[list[str]],
        *,
        no_cache: bool = False,
        reporter_type: type[_RecordingReporter] = _RecordingReporter,
        exact_hit_guard: bool = False,
    ) -> mock.Mock:
        logs = root / f"logs-{len(commands)}"
        logs.mkdir(exist_ok=True)
        reporter = reporter_type(logs, commands)
        stage_workspace = mock.Mock(return_value=workspace)
        discard_workspace = mock.Mock()
        with (
            mock.patch.object(container, "ROOT", root),
            mock.patch("fplinux_cli.output.RunReporter.create", return_value=reporter),
            mock.patch.object(
                container,
                "require_podman",
                side_effect=(
                    AssertionError("exact check hit must not require Podman")
                    if exact_hit_guard
                    else None
                ),
                return_value=None if exact_hit_guard else "podman",
            ),
            mock.patch.object(
                container,
                "load_container_lock",
                return_value={
                    "oci": {
                        "repository": "localhost/fplinux-build",
                        "platform": "linux/amd64",
                    }
                },
            ),
            mock.patch.object(
                container,
                "image_ready",
                side_effect=(
                    AssertionError("exact check hit must not inspect an image")
                    if exact_hit_guard
                    else None
                ),
                return_value=None if exact_hit_guard else True,
            ),
            mock.patch.object(
                container,
                "image_identifier",
                side_effect=(
                    AssertionError("exact check hit must not inspect an image ID")
                    if exact_hit_guard
                    else None
                ),
                return_value=(None if exact_hit_guard else "sha256:" + "c" * 64),
            ),
            mock.patch.object(
                container,
                "container_image_recipe_digest",
                return_value="b" * 64,
            ),
            mock.patch.object(
                container,
                "check_orchestration_recipe_digest",
                return_value="d" * 64,
            ),
            mock.patch.object(
                container,
                "quality_workspace_snapshot",
                return_value=snapshot,
            ),
            mock.patch.object(
                container,
                "stage_quality_workspace_snapshot",
                new=(
                    mock.Mock(
                        side_effect=AssertionError("exact check hit must not stage a workspace")
                    )
                    if exact_hit_guard
                    else stage_workspace
                ),
            ),
            mock.patch.object(
                container,
                "discard_staged_quality_workspace_snapshot",
                new=discard_workspace,
            ),
            mock.patch.object(
                container,
                "setup",
                side_effect=(
                    AssertionError("exact check hit must not set up an image")
                    if exact_hit_guard
                    else None
                ),
                return_value=None,
            ),
        ):
            try:
                container.check(scopes, no_cache=no_cache)
            except RuntimeError:
                if not exact_hit_guard:
                    discard_workspace.assert_called_once_with(snapshot, workspace)
                raise
        if exact_hit_guard:
            discard_workspace.assert_not_called()
        else:
            discard_workspace.assert_called_once_with(snapshot, workspace)
        return stage_workspace

    def test_exact_success_hit_skips_workspace_and_checker(self) -> None:
        """Return a verified scope hit before workspace materialization."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            snapshot = self._snapshot()

            self._run(root, workspace, snapshot, ["c"], commands)
            self.assertEqual(len(commands), 1)

            stage_workspace = self._run(
                root,
                workspace,
                snapshot,
                ["c"],
                commands,
                exact_hit_guard=True,
            )
            stage_workspace.assert_not_called()
            self.assertEqual(len(commands), 1)

    def test_no_cache_bypasses_an_exact_outer_receipt(self) -> None:
        """Execute the checker when the caller explicitly ignores receipts."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            snapshot = self._snapshot()
            self._run(root, workspace, snapshot, ["c"], commands)
            self._run(root, workspace, snapshot, ["c"], commands, no_cache=True)
            self.assertEqual(len(commands), 2)

    def test_only_missing_scope_runs_in_a_mixed_selection(self) -> None:
        """Avoid rerunning a hit when another selected scope is missing."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            snapshot = self._snapshot()
            self._run(root, workspace, snapshot, ["c"], commands)
            self._run(root, workspace, snapshot, ["c", "docs"], commands)
            self.assertEqual(
                [
                    command[command.index("/workspace/scripts/check.py") + 1 :]
                    for command in commands
                ],
                [["c"], ["docs"]],
            )

    def test_changed_c_bytes_are_a_cold_miss(self) -> None:
        """Invalidate the C result when one checked source byte changes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            self._run(root, workspace, self._snapshot(), ["c"], commands)
            self._run(
                root,
                workspace,
                self._snapshot(c_source=b"int changed;\n"),
                ["c"],
                commands,
            )
            self.assertEqual(len(commands), 2)

    def test_failed_forced_rerun_keeps_last_good_success(self) -> None:
        """A failed forced rerun leaves the previous exact success usable."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            snapshot = self._snapshot()
            self._run(root, workspace, snapshot, ["c"], commands)
            with self.assertRaisesRegex(RuntimeError, "scope failed"):
                self._run(
                    root,
                    workspace,
                    snapshot,
                    ["c"],
                    commands,
                    no_cache=True,
                    reporter_type=_FailingReporter,
                )
            self._run(
                root,
                workspace,
                snapshot,
                ["c"],
                commands,
                exact_hit_guard=True,
            )
            self.assertEqual(len(commands), 2)


if __name__ == "__main__":
    unittest.main()
