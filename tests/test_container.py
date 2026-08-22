# SPDX-License-Identifier: GPL-2.0-only
"""Tests for container setup lifecycle and repository hook ownership."""

from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import config, container, output
from fplinux_cli.common import ROOT as SOURCE_ROOT
from fplinux_cli.image_state import ImageState, load_image_state


def _container_lock() -> dict[str, object]:
    """Return the smallest valid build-image input set."""
    return {
        "oci": {
            "image": "localhost/fplinux:locked",
            "platform": "linux/amd64",
            "base": f"example.invalid/base@sha256:{'a' * 64}",
            "base_created": "2026-01-01T00:00:00Z",
        },
    }


def _copy_checkout(root: Path) -> None:
    """Copy the checkout without maintaining a second recipe-input registry."""
    shutil.copytree(
        SOURCE_ROOT,
        root,
        ignore=shutil.ignore_patterns(".git", ".cache", "__pycache__"),
    )


class ContainerImageRecipeTests(unittest.TestCase):
    """Keep image identity independent from the absolute checkout directory."""

    def test_image_and_check_recipes_ignore_checkout_location(self) -> None:
        """Identical checkouts use the same logical Podman argv and recipe digests."""
        lock = _container_lock()
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            first = parent / "first"
            second = parent / "second"
            _copy_checkout(first)
            _copy_checkout(second)

            with mock.patch.object(config, "ROOT", first):
                first_arguments = config.container_image_build_arguments(lock)
                first_image = config.container_image_recipe_digest(lock)
                first_check = config.check_orchestration_recipe_digest(first_image)
            with mock.patch.object(config, "ROOT", second):
                second_arguments = config.container_image_build_arguments(lock)
                second_image = config.container_image_recipe_digest(lock)
                second_check = config.check_orchestration_recipe_digest(second_image)

        expected_arguments = (
            "--platform",
            "linux/amd64",
            "--file",
            "Containerfile",
            "--build-arg",
            f"BASE_IMAGE=example.invalid/base@sha256:{'a' * 64}",
        )
        self.assertEqual(first_arguments, expected_arguments)
        self.assertEqual(second_arguments, expected_arguments)
        self.assertEqual(first_image, second_image)
        self.assertEqual(first_check, second_check)


class SetupLifecycleTests(unittest.TestCase):
    """Keep direct setup inside the unified run metadata lifecycle."""

    def test_ready_direct_setup_finishes_its_own_reporter(self) -> None:
        """Publish successful direct-setup run metadata on an image hit."""
        lock = {
            "oci": {"image": "localhost/fplinux:locked"},
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(container, "ROOT", root),
                mock.patch.object(output, "ROOT", root),
                mock.patch.object(container, "require_podman", return_value="podman"),
                mock.patch.object(
                    container,
                    "container_image_recipe_digest",
                    return_value="a" * 64,
                ),
                mock.patch.object(container, "image_ready", return_value=True),
                mock.patch.object(
                    container,
                    "image_identifier",
                    return_value="sha256:" + "b" * 64,
                ),
                mock.patch.object(container, "install_git_hooks"),
            ):
                state = container.setup(lock=lock)
            self.assertEqual(
                state,
                ImageState("a" * 64, "sha256:" + "b" * 64),
            )
            self.assertEqual(load_image_state(root / ".cache", "a" * 64), state)
            metadata_paths = list((root / ".cache/logs/setup").glob("*/run.json"))
            self.assertEqual(len(metadata_paths), 1)
            self.assertEqual(
                json.loads(metadata_paths[0].read_text())["status"],
                "success",
            )

    def test_setup_passes_exact_podman_build_argv_to_stage(self) -> None:
        """Pass production build arguments to the mocked execution stage unchanged."""
        reporter = mock.Mock()
        stage = mock.Mock()
        stage_context = mock.MagicMock()
        stage_context.__enter__.return_value = stage
        reporter.stage.return_value = stage_context
        lock = _container_lock()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(container, "ROOT", root),
                mock.patch.object(container, "require_podman", return_value="podman"),
                mock.patch.object(
                    container,
                    "container_image_recipe_digest",
                    return_value="a" * 64,
                ),
                mock.patch.object(container, "image_ready", side_effect=(False, True)),
                mock.patch.object(
                    container,
                    "image_identifier",
                    side_effect=(None, "sha256:" + "b" * 64),
                ),
                mock.patch.object(container, "install_git_hooks"),
            ):
                container.setup(reporter=reporter, lock=lock)

        stage.run.assert_called_once_with(
            [
                "podman",
                "build",
                "--platform",
                "linux/amd64",
                "--file",
                "Containerfile",
                "--build-arg",
                f"BASE_IMAGE=example.invalid/base@sha256:{'a' * 64}",
                "--tag",
                "localhost/fplinux:locked",
                "--label",
                f"org.fplinux.container.image-recipe={'a' * 64}",
                ".",
            ],
            cwd=root,
        )

    def test_podman_bare_image_id_is_normalized(self) -> None:
        """Accept the 64-hex ID emitted by the installed Podman inspect command."""
        identifier = "b" * 64
        with (
            mock.patch.object(container, "image_exists", return_value=True),
            mock.patch(
                "fplinux_cli.container.subprocess.run",
                return_value=subprocess.CompletedProcess(
                    ["podman", "image", "inspect"],
                    0,
                    f"{identifier}\n",
                    "",
                ),
            ),
        ):
            self.assertEqual(
                container.image_identifier("podman", "localhost/fplinux:locked"),
                f"sha256:{identifier}",
            )


class GitHookPathTests(unittest.TestCase):
    """Accept only paths that resolve to this checkout's owned hook directory."""

    def test_absolute_repository_hook_path_is_equivalent(self) -> None:
        """Accept the absolute spelling of this checkout's hook directory."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / ".githooks").mkdir()
            commands: list[list[str]] = []

            def fake_run(
                command: list[str], **_kwargs: object
            ) -> subprocess.CompletedProcess[str]:
                commands.append(command)
                if command[1:3] == ["rev-parse", "--show-toplevel"]:
                    return subprocess.CompletedProcess(command, 0, f"{root}\n", "")
                if command[1:5] == ["config", "--local", "--get", "core.hooksPath"]:
                    return subprocess.CompletedProcess(
                        command,
                        0,
                        f"{root / '.githooks'}\n",
                        "",
                    )
                raise AssertionError(f"unexpected Git mutation: {command}")

            with (
                mock.patch.object(container, "ROOT", root),
                mock.patch("fplinux_cli.container.shutil.which", return_value="git"),
                mock.patch("fplinux_cli.container.subprocess.run", side_effect=fake_run),
            ):
                container.install_git_hooks()
            self.assertEqual(len(commands), 2)

    def test_different_absolute_hook_path_is_rejected_without_mutation(self) -> None:
        """Reject another hook owner without rewriting the Git configuration."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            commands: list[list[str]] = []

            def fake_run(
                command: list[str], **_kwargs: object
            ) -> subprocess.CompletedProcess[str]:
                commands.append(command)
                if command[1:3] == ["rev-parse", "--show-toplevel"]:
                    return subprocess.CompletedProcess(command, 0, f"{root}\n", "")
                foreign = root.parent / "foreign-hooks"
                return subprocess.CompletedProcess(command, 0, f"{foreign}\n", "")

            with (
                mock.patch.object(container, "ROOT", root),
                mock.patch("fplinux_cli.container.shutil.which", return_value="git"),
                mock.patch("fplinux_cli.container.subprocess.run", side_effect=fake_run),
                self.assertRaisesRegex(SystemExit, "core.hooksPath is already set"),
            ):
                container.install_git_hooks()
            self.assertEqual(len(commands), 2)


if __name__ == "__main__":
    unittest.main()
