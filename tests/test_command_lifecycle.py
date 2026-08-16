# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for immutable command resolution and exact build orchestration."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import commands, config
from fplinux_cli.bundle_state import CurrentBundle
from fplinux_cli.workspace import WorkspaceSnapshot


class CommandLifecycleTests(unittest.TestCase):
    """Keep cache hits and readers ahead of every mutable or external action."""

    def setUp(self) -> None:
        """Create one exact immutable bundle fixture for each test."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bundle_path = self.root / ".cache/out/phone/bundles/default" / ("a" * 64)
        self.bundle_path.mkdir(parents=True)
        image = self.bundle_path / "image/ramboot.bin"
        image.parent.mkdir(parents=True)
        image.write_bytes(b"ramboot\n")
        self.bundle = CurrentBundle(
            path=self.bundle_path,
            generation="a" * 64,
            manifest_sha256="b" * 64,
            manifest_bytes=json.dumps(
                {
                    "workspace_digest": "c" * 64,
                    "container_image_recipe": "e" * 64,
                    "device_identity": "9" * 64,
                    "buildroot_receipt": {"recipe": "f" * 64, "sha256": "0" * 64},
                    "files": {
                        "image/ramboot.bin": {
                            "mode": 420,
                            "size": 8,
                            "sha256": hashlib.sha256(b"ramboot\n").hexdigest(),
                        }
                    },
                }
            ).encode(),
        )
        self.snapshot = WorkspaceSnapshot((), "c" * 64)

    def test_exact_build_hit_logs_without_podman_or_staging(self) -> None:
        """A valid exact generation records the invocation but starts no build work."""
        current = (self.bundle, json.loads(self.bundle.manifest_bytes))
        reporter = mock.Mock()
        with (
            mock.patch.object(commands, "load_target", return_value={"profile": "default"}),
            mock.patch.object(
                commands,
                "load_release",
                return_value={"image": "image/ramboot.bin"},
            ),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(commands, "_matching_target_bundle", return_value=current),
            mock.patch.object(commands, "_print_build_result") as print_result,
            mock.patch("fplinux_cli.commands.RunReporter.create", return_value=reporter),
            mock.patch.object(
                commands,
                "require_podman",
                side_effect=AssertionError("cache hit must not inspect Podman"),
            ),
            mock.patch.object(
                commands,
                "stage_workspace_snapshot",
                side_effect=AssertionError("cache hit must not stage a workspace"),
            ),
        ):
            commands.build("phone", 8, verbose=True)

        print_result.assert_called_once_with(
            "phone",
            self.bundle,
            {"image": "image/ramboot.bin"},
            cached=True,
        )
        reporter.finish.assert_called_once_with()

    def test_jobs_do_not_change_an_exact_bundle_hit(self) -> None:
        """Treat ``--jobs`` as scheduling rather than artifact identity."""
        with mock.patch.object(commands, "resolve_current_bundle", return_value=self.bundle):
            self.assertIsNotNone(
                commands._matching_target_bundle(  # noqa: SLF001
                    "phone",
                    {"profile": "default"},
                    self.snapshot,
                    "e" * 64,
                    "image/ramboot.bin",
                )
            )

    def test_corrupted_bundle_image_is_not_an_exact_hit(self) -> None:
        """A bundle whose image bytes drifted from the manifest is rebuilt."""
        (self.bundle_path / "image/ramboot.bin").write_bytes(b"corrupt\n")
        with mock.patch.object(commands, "resolve_current_bundle", return_value=self.bundle):
            self.assertIsNone(
                commands._matching_target_bundle(  # noqa: SLF001
                    "phone",
                    {"profile": "default"},
                    self.snapshot,
                    "e" * 64,
                    "image/ramboot.bin",
                )
            )

    def test_build_miss_requires_host_validation_after_container_success(self) -> None:
        """Container exit zero is insufficient without an exact published generation."""
        workspace = self.root / ".cache/workspaces/current"
        workspace.mkdir(parents=True)
        logs = self.root / ".cache/logs/build/run"
        logs.mkdir(parents=True)
        workspace_context = mock.MagicMock()
        workspace_context.__enter__.return_value = mock.Mock()
        container_stage = mock.Mock()
        container_context = mock.MagicMock()
        container_context.__enter__.return_value = container_stage
        reporter = mock.Mock(root=logs)
        reporter.stage.side_effect = [workspace_context, container_context]
        reporter.container_environment.return_value = {"FPLINUX_LOG_ROOT": "/logs"}
        lock = {
            "oci": {
                "image": "localhost/fplinux:locked",
                "platform": "linux/amd64",
            }
        }
        matcher = mock.Mock(side_effect=(None, None))
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value={"profile": "default"}),
            mock.patch.object(
                commands,
                "load_release",
                return_value={"image": "image/ramboot.bin"},
            ),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(commands, "load_container_lock", return_value=lock),
            mock.patch.object(
                commands,
                "container_image_recipe_digest",
                return_value="e" * 64,
            ),
            mock.patch.object(commands, "_matching_target_bundle", matcher),
            mock.patch("fplinux_cli.commands.RunReporter.create", return_value=reporter),
            mock.patch.object(commands, "require_podman", return_value="podman"),
            mock.patch.object(commands, "image_ready", return_value=True),
            mock.patch.object(
                commands,
                "stage_workspace_snapshot",
                return_value=workspace,
            ),
            self.assertRaisesRegex(
                SystemExit,
                "without publishing an exact valid current bundle",
            ),
        ):
            commands.build("phone", 4)

        self.assertEqual(matcher.call_count, 2)
        container_stage.run.assert_called_once()
        self.assertEqual(container_stage.run.call_args.kwargs, {})
        reporter.finish.assert_not_called()

    def test_run_executes_a_runner_from_the_resolved_generation(self) -> None:
        """Resolve current once and preserve that immutable generation path."""
        runner = self.bundle_path / "runner/run.py"
        runner.parent.mkdir()
        runner.write_text("#!/usr/bin/env python3\n")
        runner.chmod(0o755)
        resolver = mock.Mock(return_value=(self.bundle, {}))
        with (
            mock.patch.object(commands, "load_target", return_value={"profile": "default"}),
            mock.patch.object(commands, "_resolve_target_bundle", resolver),
            mock.patch("fplinux_cli.commands.os.execv") as execute,
        ):
            commands.run_target("phone")

        resolver.assert_called_once_with("phone", {"profile": "default"})
        execute.assert_called_once_with(os.fsencode(runner), [os.fsencode(runner)])

    def test_verify_rejects_nonzero_console_status_even_with_matching_stdout(self) -> None:
        """Console transport failure cannot be promoted to a successful verify."""
        client = self.bundle_path / "host/fplinux-usb-console"
        client.parent.mkdir()
        client.write_text("client\n")
        client.chmod(0o755)
        manifest = json.loads(self.bundle.manifest_bytes)
        target_config = {
            "profile": "default",
            "runtime": {
                "usb": {
                    "linux_console": {
                        "vendor_id": 0x1782,
                        "product_id": 0x4D00,
                        "wait_seconds": 10,
                    }
                }
            },
        }
        result = subprocess.CompletedProcess(
            [],
            7,
            stdout=f"buildroot={'f' * 64}\n6.12-fplinux-{'9' * 16}\n",
            stderr="transport failed\n",
        )
        resolver = mock.Mock(return_value=(self.bundle, manifest))
        with (
            mock.patch.object(commands, "load_target", return_value=target_config),
            mock.patch.object(commands, "_resolve_target_bundle", resolver),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(
                commands,
                "container_image_recipe_digest",
                return_value="e" * 64,
            ),
            mock.patch("fplinux_cli.commands.subprocess.run", return_value=result),
            self.assertRaisesRegex(SystemExit, "console client failed with exit status 7"),
        ):
            commands.verify_booted("phone")
        resolver.assert_called_once()

    def test_verify_matches_the_device_identity_not_the_workspace_digest(self) -> None:
        """Compare uname with the content-derived kernel suffix recorded by the bundle."""
        client = self.bundle_path / "host/fplinux-usb-console"
        client.parent.mkdir()
        client.write_text("client\n")
        client.chmod(0o755)
        manifest = json.loads(self.bundle.manifest_bytes)
        target_config = {
            "profile": "default",
            "runtime": {
                "usb": {
                    "linux_console": {
                        "vendor_id": 0x1782,
                        "product_id": 0x4D00,
                        "wait_seconds": 10,
                    }
                }
            },
        }
        result = subprocess.CompletedProcess(
            [],
            0,
            stdout=f"buildroot={'f' * 64}\n6.12-fplinux-{'9' * 16}\n",
            stderr="",
        )
        with (
            mock.patch.object(commands, "load_target", return_value=target_config),
            mock.patch.object(
                commands,
                "_resolve_target_bundle",
                return_value=(self.bundle, manifest),
            ),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(commands, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch("fplinux_cli.commands.subprocess.run", return_value=result),
            mock.patch("builtins.print") as output,
        ):
            commands.verify_booted("phone")

        output.assert_called_once_with(
            "verify: the phone runs the current build (9999999999999999)"
        )

    def test_build_argv_has_only_exact_nonoverlapping_mount_roots(self) -> None:
        """Do not expose the cache root or an ancestor alias to the build container."""
        roots = {
            "workspace": self.root / "workspace",
            "downloads": self.root / "cache/downloads",
            "ccache": self.root / "cache/ccache",
            "toolchains": self.root / "cache/toolchains",
            "linux": self.root / "cache/linux",
            "output": self.root / "cache/out",
            "logs": self.root / "cache/logs/build/run",
        }
        with mock.patch.dict(os.environ, {}, clear=True):
            command = commands._build_container_command(  # noqa: SLF001
                "/usr/bin/podman",
                target="phone",
                jobs=6,
                platform="linux/amd64",
                image="localhost/fplinux-build:locked",
                snapshot=self.snapshot,
                **roots,
                log_environment={"FPLINUX_LOG_ROOT": "/logs"},
                image_recipe="e" * 64,
            )

        mounts = [command[index + 1] for index, value in enumerate(command) if value == "--volume"]
        self.assertEqual(
            mounts,
            [
                f"{roots['downloads']}:/cache/downloads:rw,Z",
                f"{roots['ccache']}:/cache/ccache:rw,Z",
                f"{roots['toolchains']}:/cache/toolchains:rw,Z",
                f"{roots['linux']}:/cache/linux:rw,Z",
                f"{roots['output']}:/out:rw,Z",
                f"{roots['logs']}:/logs:rw,Z",
                f"{roots['workspace']}:/workspace:ro,Z",
            ],
        )
        self.assertNotIn(f"{self.root / 'cache'}:/cache:rw,Z", command)
        self.assertIn("--read-only", command)
        self.assertFalse(any(mount.split(":", 2)[1] == "/cache" for mount in mounts))
        self.assertEqual(
            command[-8:],
            [
                "localhost/fplinux-build:locked",
                "python3",
                "-m",
                "fplinux_cli.builder",
                "--target",
                "phone",
                "--jobs",
                "6",
            ],
        )
        self.assertEqual(
            command[:2],
            ["/usr/bin/podman", "run"],
        )


class ContainerRecipeTests(unittest.TestCase):
    """Keep image and cached-check identities limited to causal inputs."""

    IMAGE_INPUTS = (
        ".containerignore",
        "Containerfile",
        "container.lock.toml",
        "package.json",
        "package-lock.json",
        "requirements.lock",
    )
    CHECK_INPUTS = (
        "scripts/fplinux_cli/checkreceipts.py",
        "scripts/fplinux_cli/common.py",
        "scripts/fplinux_cli/config.py",
        "scripts/fplinux_cli/container.py",
        "scripts/fplinux_cli/image_state.py",
        "scripts/fplinux_cli/output.py",
        "scripts/fplinux_cli/workspace.py",
    )

    def setUp(self) -> None:
        """Create exact image and check-orchestration input fixtures."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        paths = (*self.IMAGE_INPUTS, *self.CHECK_INPUTS, "scripts/fplinux_cli/prune.py")
        for relative in paths:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(relative.encode())
        self.lock = {
            "oci": {
                "platform": "linux/amd64",
                "base": "example.invalid/base@sha256:" + "a" * 64,
                "debian_snapshot": "20260815T000000Z",
            },
            "buildroot": {
                "version": "2026.05.1",
                "url": "https://example.invalid/buildroot.tar.xz",
                "sha256": "b" * 64,
            },
        }

    def test_check_implementation_edit_does_not_rebuild_image(self) -> None:
        """Keep host check implementation out of the OCI image recipe."""
        with (
            mock.patch.object(config, "ROOT", self.root),
            mock.patch.object(config, "load_container_lock", return_value=self.lock),
        ):
            image_before = config.container_image_recipe_digest()
            check_before = config.check_orchestration_recipe_digest()
            (self.root / "scripts/fplinux_cli/container.py").write_bytes(b"changed\0bytes")
            image_after = config.container_image_recipe_digest()
            check_after = config.check_orchestration_recipe_digest()

        self.assertEqual(image_before, image_after)
        self.assertNotEqual(check_before, check_after)

    def test_prune_edit_does_not_invalidate_check_receipts(self) -> None:
        """Keep unrelated cache-management code out of check identities."""
        with (
            mock.patch.object(config, "ROOT", self.root),
            mock.patch.object(config, "load_container_lock", return_value=self.lock),
        ):
            before = config.check_orchestration_recipe_digest()
            (self.root / "scripts/fplinux_cli/prune.py").write_bytes(b"changed\n")
            after = config.check_orchestration_recipe_digest()

        self.assertEqual(before, after)

    def test_image_input_mode_changes_both_recipes(self) -> None:
        """Treat an OCI input mode change as causal for both identities."""
        path = self.root / "package-lock.json"
        with (
            mock.patch.object(config, "ROOT", self.root),
            mock.patch.object(config, "load_container_lock", return_value=self.lock),
        ):
            image_before = config.container_image_recipe_digest()
            check_before = config.check_orchestration_recipe_digest()
            path.chmod(0o755)
            image_after = config.container_image_recipe_digest()
            check_after = config.check_orchestration_recipe_digest()

        self.assertNotEqual(image_before, image_after)
        self.assertNotEqual(check_before, check_after)


if __name__ == "__main__":
    unittest.main()
