# SPDX-License-Identifier: GPL-2.0-only
"""Tests for container setup lifecycle and repository hook ownership."""

from __future__ import annotations

import contextlib
import io
import json
import os
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.common import ROOT as SOURCE_ROOT
from fplinux_cli.environment import doctor as env_doctor
from fplinux_cli.environment import git_hooks, image_store, images, kern
from fplinux_cli.environment import setup as env_setup
from fplinux_cli.environment.image_state import ImageState, load_image_state
from fplinux_cli.quality import receipts as checkreceipts
from fplinux_cli.reporting import run as output


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


class ContainerImageRecipeTests:
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

            with mock.patch.object(common, "ROOT", first):
                first_arguments = images.container_image_build_arguments(lock)
                first_image = images.container_image_recipe_digest(lock)
                first_check = checkreceipts.check_orchestration_recipe_digest(first_image)
            with mock.patch.object(common, "ROOT", second):
                second_arguments = images.container_image_build_arguments(lock)
                second_image = images.container_image_recipe_digest(lock)
                second_check = checkreceipts.check_orchestration_recipe_digest(second_image)

        expected_arguments = (
            "-f",
            "Containerfile",
            "--build-arg",
            f"BASE_IMAGE=localhost/fplinux-alpine-base:3.24.1-{'e' * 64}",
            "--build-arg",
            "FPLINUX_OFFLINE=1",
        )
        assert (first_arguments) == (expected_arguments)
        assert (second_arguments) == (expected_arguments)
        assert (first_image) == (second_image)
        assert (first_check) == (second_check)

    def test_image_references_are_bound_to_exact_recipe_inputs(self) -> None:
        """Both local tags change only when their exact causal input changes."""
        lock = _container_lock()
        assert (images.container_base_image_reference(lock)) == (
            f"localhost/fplinux-alpine-base:3.24.1-{'e' * 64}"
        )
        assert (images.container_image_reference(lock, "a" * 64)) == (
            f"localhost/fplinux-build:{'a' * 64}"
        )

    def test_runtime_binary_and_content_are_causal_image_inputs(self) -> None:
        """Changing pinned runtime bytes or installed content invalidates reuse."""
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
            with mock.patch.object(common, "ROOT", root):
                recipe = images.container_image_recipe_digest(lock)
                runtime_changed = images.container_image_recipe_digest(changed_runtime)
        assert (recipe) != (runtime_changed)
        assert (images.container_artifact_recipe_digest(recipe, "a" * 64)) != (
            images.container_artifact_recipe_digest(recipe, "b" * 64)
        )

    def test_host_terminal_patch_invalidates_image_but_target_aport_does_not(self) -> None:
        """Host tests must use the patched terminal engine matching the target build."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            _copy_checkout(root)
            with mock.patch.object(common, "ROOT", root):
                before = images.container_image_recipe_digest(_container_lock())
                aport = root / "alpine/aports/fplinux-terminal/APKBUILD"
                aport.write_text(aport.read_text() + "\n# Target-only package change\n")
                assert (images.container_image_recipe_digest(_container_lock())) == (before)
                patch = root / "alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch"
                patch.write_text(patch.read_text() + "\n")
                assert (images.container_image_recipe_digest(_container_lock())) != (before)


class SetupLifecycleTests:
    """Keep direct setup inside the unified run metadata lifecycle."""

    def test_ready_direct_setup_finishes_its_own_reporter(self) -> None:
        """Publish successful direct-setup run metadata on an image hit."""
        lock = _container_lock()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(env_setup, "ROOT", root),
                mock.patch.object(kern, "ROOT", root),
                mock.patch.object(image_store, "ROOT", root),
                mock.patch.object(output, "ROOT", root),
                mock.patch.object(env_setup, "install_kern", return_value="/cache/kern"),
                mock.patch.object(env_setup, "environment_inputs", return_value=[]),
                mock.patch.object(
                    env_setup,
                    "container_image_recipe_digest",
                    return_value="a" * 64,
                ),
                mock.patch.object(
                    env_setup,
                    "current_image_state",
                    return_value=ImageState("a" * 64, "b" * 64, "b" * 64),
                ) as current_image,
                mock.patch.object(image_store, "current_image_state", new=current_image),
                mock.patch.object(env_setup, "prune_build_history"),
                mock.patch.object(env_setup, "discard_transient_images"),
                mock.patch.object(env_setup, "discard_obsolete_images"),
                mock.patch.object(env_setup, "install_git_hooks"),
            ):
                state = env_setup.setup(lock=lock)
            assert (state) == (ImageState("a" * 64, "b" * 64, "b" * 64))
            assert (load_image_state(root / ".cache", "a" * 64)) == (state)
            metadata_paths = list((root / ".cache/logs/setup").glob("*/run.json"))
            assert (len(metadata_paths)) == (1)
            assert (json.loads(metadata_paths[0].read_text())["status"]) == ("success")

    def test_setup_passes_exact_kern_build_boundary_to_stage(self) -> None:
        """Build the recipe-addressed tag through the pinned project-local binary."""
        reporter = mock.Mock()
        stage = mock.Mock()
        stage_context = mock.MagicMock()
        stage_context.__enter__.return_value = stage
        reporter.stage.return_value = stage_context
        lock = _container_lock()
        staged_recipe: dict[str, bytes] = {}

        def collect_context(_command: list[str], *, cwd: Path, **_kwargs: object) -> None:
            staged_recipe["Containerfile"] = (cwd / "Containerfile").read_bytes()
            staged_recipe["package.json"] = (cwd / "package.json").read_bytes()
            assert (list((cwd / "inputs").iterdir())) == ([])
            assert not ((cwd / ".cache").exists())

        stage.run.side_effect = collect_context
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative in (
                ".kernignore",
                "Containerfile",
                "scripts/fplinux_cli/environment/image_content.py",
                "package.json",
                "package-lock.json",
                "alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch",
            ):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(relative, encoding="utf-8")
            with (
                mock.patch.object(env_setup, "ROOT", root),
                mock.patch.object(kern, "ROOT", root),
                mock.patch.object(image_store, "ROOT", root),
                mock.patch.object(env_setup, "install_kern", return_value="/cache/kern"),
                mock.patch.object(env_setup, "environment_inputs", return_value=[]),
                mock.patch.object(
                    env_setup,
                    "container_image_recipe_digest",
                    return_value="a" * 64,
                ),
                mock.patch.object(
                    env_setup,
                    "current_image_state",
                    side_effect=(None, ImageState("a" * 64, "b" * 64, "b" * 64)),
                ) as current_image,
                mock.patch.object(image_store, "current_image_state", new=current_image),
                mock.patch.object(env_setup, "base_image_ready", return_value=True),
                mock.patch.object(env_setup, "prune_build_history"),
                mock.patch.object(env_setup, "discard_transient_images"),
                mock.patch.object(env_setup, "discard_obsolete_images"),
                mock.patch.object(
                    env_setup,
                    "image_metadata",
                    return_value=("a" * 64, "b" * 64, "b" * 64),
                ),
                mock.patch.object(env_setup, "publish_staged_image"),
                mock.patch(
                    "fplinux_cli.environment.setup.secrets.token_hex", return_value="b" * 64
                ),
                mock.patch.object(env_setup, "install_git_hooks"),
            ):
                env_setup.setup(reporter=reporter, lock=lock)

        call = stage.run.call_args
        assert (call) is not None
        command = call.args[0]
        assert (command[:2]) == (["/cache/kern", "build"])
        assert command[command.index("-t") + 1].startswith(
            f"localhost/fplinux-build:{'a' * 64}-staging-{os.getpid()}-"
        )
        assert (command[command.index("-f") + 1]) == ("Containerfile")
        build_arguments = {
            command[index + 1]
            for index, value in enumerate(command[:-1])
            if value == "--build-arg"
        }
        assert (build_arguments) == (
            {
                f"BASE_IMAGE=localhost/fplinux-alpine-base:3.24.1-{'e' * 64}",
                "FPLINUX_OFFLINE=1",
                f"FPLINUX_IMAGE_RECIPE={'a' * 64}",
                f"FPLINUX_IMAGE_GENERATION={'b' * 64}",
            }
        )
        assert (command[-1]) == (".")
        assert (staged_recipe["Containerfile"]) == (b"Containerfile")
        assert (staged_recipe["package.json"]) == (b"package.json")
        assert not (call.kwargs["cwd"].exists())
        assert (call.kwargs["timeout"]) == (2 * 60 * 60)
        assert (call.kwargs["env"]["XDG_CACHE_HOME"]) == (str(root / ".cache/kern/cache"))
        assert (call.kwargs["env"]["XDG_DATA_HOME"]) == (str(root / ".cache/kern/data"))
        assert (call.kwargs["env"]["XDG_CONFIG_HOME"]) == (str(root / ".cache/kern/config"))

    def test_kern_image_probe_timeout_is_reported(self) -> None:
        """A stuck runtime lookup fails with its named boundary instead of hanging."""
        with (
            mock.patch.object(image_store, "kern_environment", return_value={}),
            mock.patch(
                "fplinux_cli.environment.image_store.subprocess.run",
                side_effect=subprocess.TimeoutExpired(["kern", "box"], 60),
            ),
            pytest.raises(SystemExit, match="Kern image lookup timed out"),
        ):
            image_store.image_generation("kern", "localhost/fplinux:locked")

    def test_image_generation_is_read_from_the_built_image_marker(self) -> None:
        """Use the build-published generation rather than Kern's private store layout."""
        generation = "b" * 64
        with (
            mock.patch.object(image_store, "kern_environment", return_value={}),
            mock.patch(
                "fplinux_cli.environment.image_store.subprocess.run",
                return_value=subprocess.CompletedProcess(
                    ["kern", "box"],
                    0,
                    f"{'a' * 64}\n{generation}\n{'b' * 64}\n{'b' * 64}\n",
                    "",
                ),
            ),
        ):
            assert (image_store.image_generation("kern", "localhost/fplinux:locked")) == (
                generation
            )


class KernRetentionTests:
    """Bound project-owned provider state without touching unrelated images or files."""

    def test_obsolete_project_images_exclude_current_and_unrelated_references(self) -> None:
        """Remove only superseded FPLinux tags through Kern's image API."""
        lock = _container_lock()
        recipe = "a" * 64
        current_base = images.container_base_image_reference(lock)
        current_build = images.container_image_reference(lock, recipe)
        stale = "localhost/fplinux-build:" + "b" * 64
        remove = mock.Mock()
        with (
            mock.patch.object(
                image_store,
                "image_references",
                return_value=frozenset(
                    {current_base, current_build, stale, "localhost/unrelated:keep"}
                ),
            ),
            mock.patch.object(image_store, "remove_images", new=remove),
        ):
            image_store.discard_obsolete_images("kern", lock, recipe)

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
            mock.patch.object(image_store, "image_references", side_effect=references),
            mock.patch.object(
                image_store,
                "temporary_image_reference",
                return_value=backup,
            ),
            mock.patch.object(image_store, "tag_image", side_effect=tag),
            mock.patch.object(image_store, "remove_images", side_effect=remove),
            pytest.raises(SystemExit, match="tag failed"),
        ):
            image_store.publish_staged_image(
                "kern",
                staging,
                destination,
                lambda _image: True,
            )

        assert (images) == ({destination: "last-good"})


class GitHookPathTests:
    """Preserve hook ownership and bounded Git query failures."""

    def test_git_hook_timeout_is_reported_without_mutation(self) -> None:
        """A stuck Git query fails before writing repository configuration."""
        with (
            mock.patch("fplinux_cli.environment.git_hooks.shutil.which", return_value="git"),
            mock.patch(
                "fplinux_cli.environment.git_hooks.subprocess.run",
                side_effect=subprocess.TimeoutExpired(["git", "rev-parse"], 60),
            ),
            pytest.raises(SystemExit, match="Git hook configuration timed out"),
        ):
            git_hooks.install_git_hooks()

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
                mock.patch.object(git_hooks, "ROOT", root),
                mock.patch("fplinux_cli.environment.git_hooks.shutil.which", return_value="git"),
                mock.patch(
                    "fplinux_cli.environment.git_hooks.subprocess.run", side_effect=fake_run
                ),
            ):
                git_hooks.install_git_hooks()
            assert (len(commands)) == (2)

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
                if command[1:] == ["worktree", "list", "--porcelain", "-z"]:
                    return subprocess.CompletedProcess(command, 0, f"worktree {root}\0\0", "")
                foreign = root.parent / "foreign-hooks"
                return subprocess.CompletedProcess(command, 0, f"{foreign}\n", "")

            with (
                mock.patch.object(git_hooks, "ROOT", root),
                mock.patch("fplinux_cli.environment.git_hooks.shutil.which", return_value="git"),
                mock.patch(
                    "fplinux_cli.environment.git_hooks.subprocess.run", side_effect=fake_run
                ),
                pytest.raises(SystemExit, match=r"core.hooksPath is already set"),
            ):
                git_hooks.install_git_hooks()
            assert (len(commands)) == (3)


class DoctorDiagnosticsTests:
    """Keep environment diagnostics aggregated and consistently formatted."""

    def test_missing_runtime_and_unsupported_host_are_both_reported(self) -> None:
        """A host with two problems reports both through stderr and exits once."""
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(platform, "system", return_value="Linux"),
            mock.patch.object(platform, "machine", return_value="aarch64"),
            mock.patch.object(env_doctor, "load_container_lock", return_value={}),
            mock.patch.object(env_doctor, "kern_available", return_value=False),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
            pytest.raises(SystemExit) as raised,
        ):
            env_doctor.doctor()
        assert (raised.value.code) == (1)
        assert ("linux/amd64") in (stderr.getvalue())
        assert ("Kern is not ready") in (stderr.getvalue())
        assert all(line.startswith("fplinux: ") for line in stderr.getvalue().splitlines())
        assert ("doctor: OK") not in (stdout.getvalue())
