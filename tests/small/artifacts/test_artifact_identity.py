# SPDX-License-Identifier: GPL-2.0-only
"""Host contracts for stable content identity and operational image state."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.cli.bundles import build_identity, manifest_matches_identity
from fplinux_cli.environment import image_store as kern
from fplinux_cli.environment.image_content import image_content_digest
from fplinux_cli.environment.image_state import ImageState, load_image_state, publish_image_state
from fplinux_cli.environment.images import container_artifact_recipe_digest
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests.small.artifacts.test_image_content import installed_tree

if TYPE_CHECKING:
    from pathlib import Path


class ArtifactIdentityTests:
    """Content governs artifact reuse while generation identifies a setup invocation."""

    @staticmethod
    def test_new_generation_of_identical_content_keeps_artifact_reuse(tmp_path: Path) -> None:
        """An operational setup refresh alone does not make a bundle stale."""
        cache = tmp_path
        keys = cache / "apk-signing"
        keys.mkdir()
        (keys / "fplinux-build.rsa.pub").write_bytes(b"admitted public key\n")
        snapshot = WorkspaceSnapshot((), "a" * 64)
        first = ImageState("b" * 64, "c" * 64, "d" * 64)
        second = ImageState("b" * 64, "e" * 64, "d" * 64)
        publish_image_state(cache, first)
        initial = build_identity(snapshot, load_image_state(cache, "b" * 64), cache)
        publish_image_state(cache, second)
        replacement = build_identity(snapshot, load_image_state(cache, "b" * 64), cache)
        assert (load_image_state(cache, "b" * 64)) == (second)
        assert (initial) == (replacement)
        if initial is None:
            pytest.fail("the complete fixture must produce a build identity")
        manifest = {
            "workspace_digest": initial.workspace_digest,
            "container_image_recipe": initial.container_image_recipe,
            "container_image_content": initial.container_image_content,
            "apk_signing_key": initial.apk_signing_key,
        }
        assert manifest_matches_identity(manifest, replacement)

    @staticmethod
    @pytest.mark.parametrize("change", ["bytes", "mode"])
    def test_actual_content_change_rejects_previous_bundle(tmp_path: Path, change: str) -> None:
        """Replacing installed compiler bytes or mode rejects the old artifact identity."""
        cache = tmp_path / "cache"
        cache.mkdir()
        image_root = tmp_path / "image"
        compiler = installed_tree(image_root)
        keys = cache / "apk-signing"
        keys.mkdir()
        (keys / "fplinux-build.rsa.pub").write_bytes(b"admitted public key\n")
        snapshot = WorkspaceSnapshot((), "a" * 64)
        first = ImageState("b" * 64, "c" * 64, image_content_digest(image_root))
        if change == "bytes":
            compiler.write_bytes(b"updated compiler\n")
        else:
            compiler.chmod(0o644)
        changed = ImageState("b" * 64, "c" * 64, image_content_digest(image_root))
        original = build_identity(snapshot, first, cache)
        replacement = build_identity(snapshot, changed, cache)
        if original is None:
            pytest.fail("the complete fixture must produce a build identity")
        manifest = {
            "workspace_digest": original.workspace_digest,
            "container_image_recipe": original.container_image_recipe,
            "container_image_content": original.container_image_content,
            "apk_signing_key": original.apk_signing_key,
        }
        assert not (manifest_matches_identity(manifest, replacement))
        assert (container_artifact_recipe_digest("b" * 64, first.image_content)) != (
            container_artifact_recipe_digest("b" * 64, changed.image_content)
        )

    @staticmethod
    def test_signing_context_is_an_independent_artifact_input(tmp_path: Path) -> None:
        """Changing the admitted public key requires rebuilding signed artifacts."""
        cache = tmp_path
        keys = cache / "apk-signing"
        keys.mkdir()
        public_key = keys / "fplinux-build.rsa.pub"
        public_key.write_bytes(b"first admitted public key\n")
        snapshot = WorkspaceSnapshot((), "a" * 64)
        state = ImageState("b" * 64, "c" * 64, "d" * 64)
        first = build_identity(snapshot, state, cache)
        public_key.write_bytes(b"second admitted public key\n")
        assert (first) != (build_identity(snapshot, state, cache))

    @staticmethod
    def test_previous_state_without_content_identity_is_a_cache_miss(tmp_path: Path) -> None:
        """A state that cannot identify actual installed content grants no reuse."""
        cache = tmp_path
        (cache / "host-image-state.json").write_text(
            '{"container_image_recipe":"'
            + "b" * 64
            + '","image_generation":"'
            + "c" * 64
            + '"}\n',
            encoding="utf-8",
        )
        assert (load_image_state(cache, "b" * 64)) is None

    @staticmethod
    def test_loaded_image_content_is_rechecked_beyond_its_embedded_marker(tmp_path: Path) -> None:
        """A mode-changing export cannot be admitted solely by its unchanged marker."""
        image_root = tmp_path
        compiler = installed_tree(image_root)
        declared_content = image_content_digest(image_root)
        marker = f"{'b' * 64}\n{'c' * 64}\n{declared_content}\n"
        with (
            mock.patch.object(kern, "kern_environment", return_value={}),
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess(
                    ["kern", "box"], 0, marker + declared_content + "\n", ""
                ),
            ),
        ):
            assert (kern.current_image_state("kern", "loaded-image", "b" * 64)) == (
                ImageState("b" * 64, "c" * 64, declared_content)
            )
        compiler.chmod(0o644)
        observed_content = image_content_digest(image_root)
        with (
            mock.patch.object(kern, "kern_environment", return_value={}),
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess(
                    ["kern", "box"], 0, marker + observed_content + "\n", ""
                ),
            ),
        ):
            assert (kern.current_image_state("kern", "loaded-image", "b" * 64)) is None
