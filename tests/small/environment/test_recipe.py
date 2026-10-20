# SPDX-License-Identifier: GPL-2.0-only
"""Causal contracts for environment recipes and installation resource capture."""

from __future__ import annotations

import copy
import shutil
import tempfile
from pathlib import Path

import pytest
from fplinux_cli import common
from fplinux_cli.environment import definition


@pytest.fixture
def recipe_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Copy actual causal resources while keeping unrelated project data out of the fixture."""
    source = common.ROOT
    for relative in (*definition.ENVIRONMENT_RECIPE_INPUTS, "sources.lock.toml"):
        destination = tmp_path / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / relative, destination)
    monkeypatch.setattr(common, "ROOT", tmp_path)
    return tmp_path


def test_recipe_tracks_installer_bytes_modes_and_runtime(
    recipe_checkout: Path,
) -> None:
    """Changing installation behavior or the runtime invalidates prepared-environment reuse."""
    lock = definition.load_environment_lock(recipe_checkout)
    original = definition.environment_recipe_digest(lock)
    installer = recipe_checkout / "scripts/fplinux_cli/environment/install.py"
    contents = installer.read_bytes()
    installer.write_bytes(contents + b"\n")
    changed_source = definition.environment_recipe_digest(lock)
    assert changed_source != original
    installer.chmod(0o755)
    assert definition.environment_recipe_digest(lock) != changed_source
    installer.write_bytes(contents)
    installer.chmod(0o644)
    changed_runtime = copy.deepcopy(lock)
    changed_runtime["kern"]["binary_sha256"] = "0" * 64
    assert definition.environment_recipe_digest(changed_runtime) != original


def test_same_inputs_at_another_checkout_keep_recipe(
    recipe_checkout: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Absolute source and rootfs locations do not invalidate identical environments."""
    lock = definition.load_environment_lock(recipe_checkout)
    original = definition.environment_recipe_digest(lock)
    with tempfile.TemporaryDirectory() as temporary:
        alternate = Path(temporary) / "checkout"
        shutil.copytree(recipe_checkout, alternate)
        monkeypatch.setattr(common, "ROOT", alternate)
        assert definition.environment_recipe_digest(lock) == original


def test_target_only_changes_remain_hits_but_host_terminal_patch_misses(
    recipe_checkout: Path,
) -> None:
    """Only the terminal patch consumed by host installation affects this environment."""
    lock = definition.load_environment_lock(recipe_checkout)
    original = definition.environment_recipe_digest(lock)
    target_aport = recipe_checkout / "alpine/aports/fplinux-terminal/APKBUILD"
    target_aport.parent.mkdir(parents=True)
    target_aport.write_text("target-only package edit\n")
    assert definition.environment_recipe_digest(lock) == original
    host_patch = recipe_checkout / "alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch"
    host_patch.write_bytes(host_patch.read_bytes() + b"\n")
    assert definition.environment_recipe_digest(lock) != original


def test_artifact_recipe_depends_on_actual_installed_content() -> None:
    """Two prepared environments with one declaration cannot share different tool bytes."""
    assert definition.environment_artifact_recipe_digest(
        "a" * 64, "b" * 64
    ) != definition.environment_artifact_recipe_digest("a" * 64, "c" * 64)
