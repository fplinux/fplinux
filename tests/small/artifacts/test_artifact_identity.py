# SPDX-License-Identifier: GPL-2.0-only
"""Host contracts for stable content identity and operational environment state."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.reuse import build_identity, manifest_matches_identity
from fplinux_cli.environment.definition import environment_artifact_recipe_digest
from fplinux_cli.environment.image_content import image_content_digest
from fplinux_cli.environment.state import (
    EnvironmentState,
    load_environment_state,
    publish_environment_state,
)
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests.small.artifacts.image_fixtures import installed_tree

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
        first = EnvironmentState("b" * 64, "c" * 64, "d" * 64)
        second = EnvironmentState("b" * 64, "e" * 64, "d" * 64)
        publish_environment_state(cache, first)
        initial = build_identity(snapshot, load_environment_state(cache, "b" * 64), cache)
        publish_environment_state(cache, second)
        replacement = build_identity(snapshot, load_environment_state(cache, "b" * 64), cache)
        assert (load_environment_state(cache, "b" * 64)) == (second)
        assert (initial) == (replacement)
        if initial is None:
            pytest.fail("the complete fixture must produce a build identity")
        manifest = {
            "workspace_digest": initial.workspace_digest,
            "environment_recipe": initial.environment_recipe,
            "environment_content": initial.environment_content,
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
        first = EnvironmentState("b" * 64, "c" * 64, image_content_digest(image_root))
        if change == "bytes":
            compiler.write_bytes(b"updated compiler\n")
        else:
            compiler.chmod(0o644)
        changed = EnvironmentState("b" * 64, "c" * 64, image_content_digest(image_root))
        original = build_identity(snapshot, first, cache)
        replacement = build_identity(snapshot, changed, cache)
        if original is None:
            pytest.fail("the complete fixture must produce a build identity")
        manifest = {
            "workspace_digest": original.workspace_digest,
            "environment_recipe": original.environment_recipe,
            "environment_content": original.environment_content,
            "apk_signing_key": original.apk_signing_key,
        }
        assert not (manifest_matches_identity(manifest, replacement))
        assert (environment_artifact_recipe_digest("b" * 64, first.content)) != (
            environment_artifact_recipe_digest("b" * 64, changed.content)
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
        state = EnvironmentState("b" * 64, "c" * 64, "d" * 64)
        first = build_identity(snapshot, state, cache)
        public_key.write_bytes(b"second admitted public key\n")
        assert (first) != (build_identity(snapshot, state, cache))
