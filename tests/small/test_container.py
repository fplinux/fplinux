# SPDX-License-Identifier: GPL-2.0-only
"""Tests for container setup lifecycle and repository hook ownership."""

from __future__ import annotations

import json
import os
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
    """Return the smallest valid project-local Kern and build-image input set."""
    return {
        "kern": {
            "version": "0.7.1",
            "archive_url": "https://example.invalid/kern.tar.gz",
            "archive_sha256": "c" * 64,
            "binary_sha256": "d" * 64,
        },
        "oci": {
            "repository": "localhost/fplinux-build",
            "platform": "linux/amd64",
            "base_repository": "localhost/fplinux-alpine-base",
            "base_release": "3.24.1",
            "base_rootfs_url": "https://example.invalid/alpine-minirootfs.tar.gz",
            "base_rootfs_sha256": "e" * 64,
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
        """Identical checkouts use the same logical Kern argv and recipe digests."""
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
            "-f",
            "Containerfile",
            "--build-arg",
            f"BASE_IMAGE=localhost/fplinux-alpine-base:3.24.1-{'e' * 64}",
        )
        self.assertEqual(first_arguments, expected_arguments)
        self.assertEqual(second_arguments, expected_arguments)
        self.assertEqual(first_image, second_image)
        self.assertEqual(first_check, second_check)

    def test_image_references_are_bound_to_exact_recipe_inputs(self) -> None:
        """Both local tags change only when their exact causal input changes."""
        lock = _container_lock()
        self.assertEqual(
            config.container_base_image_reference(lock),
            f"localhost/fplinux-alpine-base:3.24.1-{'e' * 64}",
        )
        self.assertEqual(
            config.container_image_reference(lock, "a" * 64),
            f"localhost/fplinux-build:{'a' * 64}",
        )

    def test_runtime_binary_and_generation_are_causal_image_inputs(self) -> None:
        """Changing either pinned runtime bytes or the built generation invalidates reuse."""
        lock = _container_lock()
        changed_runtime = _container_lock()
        changed_runtime["kern"] = {
            "version": "0.7.1",
            "archive_url": "https://example.invalid/kern.tar.gz",
            "archive_sha256": "c" * 64,
            "binary_sha256": "f" * 64,
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            _copy_checkout(root)
            with mock.patch.object(config, "ROOT", root):
                recipe = config.container_image_recipe_digest(lock)
                runtime_changed = config.container_image_recipe_digest(changed_runtime)
        self.assertNotEqual(recipe, runtime_changed)
        self.assertNotEqual(
            config.container_runtime_recipe_digest(recipe, "a" * 64),
            config.container_runtime_recipe_digest(recipe, "b" * 64),
        )


class SetupLifecycleTests(unittest.TestCase):
    """Keep direct setup inside the unified run metadata lifecycle."""

    def test_ready_direct_setup_finishes_its_own_reporter(self) -> None:
        """Publish successful direct-setup run metadata on an image hit."""
        lock = _container_lock()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(container, "ROOT", root),
                mock.patch.object(output, "ROOT", root),
                mock.patch.object(container, "_install_kern", return_value="/cache/kern"),
                mock.patch.object(
                    container,
                    "container_image_recipe_digest",
                    return_value="a" * 64,
                ),
                mock.patch.object(
                    container,
                    "current_image_state",
                    return_value=ImageState("a" * 64, "b" * 64),
                ),
                mock.patch.object(container, "_prune_kern_build_history"),
                mock.patch.object(container, "_discard_transient_kern_images"),
                mock.patch.object(container, "_discard_obsolete_kern_images"),
                mock.patch.object(container, "install_git_hooks"),
            ):
                state = container.setup(lock=lock)
            self.assertEqual(
                state,
                ImageState("a" * 64, "b" * 64),
            )
            self.assertEqual(load_image_state(root / ".cache", "a" * 64), state)
            metadata_paths = list((root / ".cache/logs/setup").glob("*/run.json"))
            self.assertEqual(len(metadata_paths), 1)
            self.assertEqual(
                json.loads(metadata_paths[0].read_text())["status"],
                "success",
            )

    def test_setup_passes_exact_kern_build_boundary_to_stage(self) -> None:
        """Build the recipe-addressed tag through the pinned project-local binary."""
        reporter = mock.Mock()
        stage = mock.Mock()
        stage_context = mock.MagicMock()
        stage_context.__enter__.return_value = stage
        reporter.stage.return_value = stage_context
        lock = _container_lock()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative in (".kernignore", "Containerfile", "package.json", "package-lock.json"):
                (root / relative).write_text(relative, encoding="utf-8")
            with (
                mock.patch.object(container, "ROOT", root),
                mock.patch.object(container, "_install_kern", return_value="/cache/kern"),
                mock.patch.object(
                    container,
                    "container_image_recipe_digest",
                    return_value="a" * 64,
                ),
                mock.patch.object(
                    container,
                    "current_image_state",
                    side_effect=(None, ImageState("a" * 64, "b" * 64)),
                ),
                mock.patch.object(container, "_base_image_ready", return_value=True),
                mock.patch.object(container, "_prune_kern_build_history"),
                mock.patch.object(container, "_discard_transient_kern_images"),
                mock.patch.object(container, "_discard_obsolete_kern_images"),
                mock.patch.object(
                    container,
                    "_image_metadata",
                    return_value=("a" * 64, "b" * 64),
                ),
                mock.patch.object(container, "_publish_staged_kern_image"),
                mock.patch("fplinux_cli.container.secrets.token_hex", return_value="b" * 64),
                mock.patch.object(container, "install_git_hooks"),
            ):
                container.setup(reporter=reporter, lock=lock)

        call = stage.run.call_args
        self.assertIsNotNone(call)
        command = call.args[0]
        self.assertEqual(command[:2], ["/cache/kern", "build"])
        self.assertTrue(
            command[command.index("-t") + 1].startswith(
                f"localhost/fplinux-build:{'a' * 64}-staging-{os.getpid()}-"
            )
        )
        self.assertEqual(command[command.index("-f") + 1], "Containerfile")
        build_arguments = {
            command[index + 1]
            for index, value in enumerate(command[:-1])
            if value == "--build-arg"
        }
        self.assertEqual(
            build_arguments,
            {
                f"BASE_IMAGE=localhost/fplinux-alpine-base:3.24.1-{'e' * 64}",
                f"FPLINUX_IMAGE_RECIPE={'a' * 64}",
                f"FPLINUX_IMAGE_GENERATION={'b' * 64}",
            },
        )
        self.assertEqual(command[-1], ".")
        self.assertEqual(call.kwargs["cwd"], root)
        self.assertEqual(call.kwargs["timeout"], 2 * 60 * 60)
        self.assertEqual(call.kwargs["env"]["XDG_CACHE_HOME"], str(root / ".cache/kern/cache"))
        self.assertEqual(call.kwargs["env"]["XDG_DATA_HOME"], str(root / ".cache/kern/data"))
        self.assertEqual(call.kwargs["env"]["XDG_CONFIG_HOME"], str(root / ".cache/kern/config"))

    def test_kern_image_probe_timeout_is_reported(self) -> None:
        """A stuck runtime lookup fails with its named boundary instead of hanging."""
        with (
            mock.patch.object(container, "kern_environment", return_value={}),
            mock.patch(
                "fplinux_cli.container.subprocess.run",
                side_effect=subprocess.TimeoutExpired(["kern", "box"], 60),
            ),
            self.assertRaisesRegex(SystemExit, "Kern image lookup timed out"),
        ):
            container.image_generation("kern", "localhost/fplinux:locked")

    def test_image_generation_is_read_from_the_built_image_marker(self) -> None:
        """Use the build-published generation rather than Kern's private store layout."""
        generation = "b" * 64
        with (
            mock.patch.object(container, "kern_environment", return_value={}),
            mock.patch(
                "fplinux_cli.container.subprocess.run",
                return_value=subprocess.CompletedProcess(
                    ["kern", "box"],
                    0,
                    f"{'a' * 64}\n{generation}\n",
                    "",
                ),
            ),
        ):
            self.assertEqual(
                container.image_generation("kern", "localhost/fplinux:locked"),
                generation,
            )


class KernRetentionTests(unittest.TestCase):
    """Bound project-owned provider state without touching unrelated images or files."""

    def test_obsolete_project_images_exclude_current_and_unrelated_references(self) -> None:
        """Remove only superseded FPLinux tags through Kern's image API."""
        lock = _container_lock()
        recipe = "a" * 64
        current_base = config.container_base_image_reference(lock)
        current_build = config.container_image_reference(lock, recipe)
        stale = "localhost/fplinux-build:" + "b" * 64
        remove = mock.Mock()
        with (
            mock.patch.object(
                container,
                "_kern_image_references",
                return_value=frozenset(
                    {current_base, current_build, stale, "localhost/unrelated:keep"}
                ),
            ),
            mock.patch.object(container, "_remove_kern_images", new=remove),
        ):
            container._discard_obsolete_kern_images("kern", lock, recipe)  # noqa: SLF001

        remove.assert_called_once_with("kern", {stale})

    def test_failed_tag_publication_restores_last_good_and_discards_private_tags(self) -> None:
        """A provider publication error leaves the previous consumer tag available."""
        staging = "localhost/fplinux-build:staging"
        destination = "localhost/fplinux-build:current"
        backup = "localhost/fplinux-build:backup"
        images = {staging: "new", destination: "last-good"}

        def references(_kern: str) -> frozenset[str]:
            return frozenset(images)

        def tag(_kern: str, source: str, target: str) -> None:
            if source == staging and target == destination:
                images.pop(destination)
                message = "tag failed"
                raise SystemExit(message)
            images[target] = images[source]

        def remove(_kern: str, selected: set[str]) -> None:
            for reference in selected:
                images.pop(reference)

        with (
            mock.patch.object(container, "_kern_image_references", side_effect=references),
            mock.patch.object(
                container,
                "_temporary_image_reference",
                return_value=backup,
            ),
            mock.patch.object(container, "_tag_kern_image", side_effect=tag),
            mock.patch.object(container, "_remove_kern_images", side_effect=remove),
            self.assertRaisesRegex(SystemExit, "tag failed"),
        ):
            container._publish_staged_kern_image(  # noqa: SLF001
                "kern",
                staging,
                destination,
                lambda _image: True,
            )

        self.assertEqual(images, {destination: "last-good"})


class GitHookPathTests(unittest.TestCase):
    """Accept only paths that resolve to this checkout's owned hook directory."""

    def test_git_hook_timeout_is_reported_without_mutation(self) -> None:
        """A stuck Git query fails before writing repository configuration."""
        with (
            mock.patch("fplinux_cli.container.shutil.which", return_value="git"),
            mock.patch(
                "fplinux_cli.container.subprocess.run",
                side_effect=subprocess.TimeoutExpired(["git", "rev-parse"], 60),
            ),
            self.assertRaisesRegex(SystemExit, "Git hook configuration timed out"),
        ):
            container.install_git_hooks()

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
