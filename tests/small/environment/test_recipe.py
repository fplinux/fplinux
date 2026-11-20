# SPDX-License-Identifier: GPL-2.0-only
"""Causal contracts for environment recipes and installation resource capture."""

from __future__ import annotations

import copy
import shutil
import tempfile
from pathlib import Path
from unittest import mock

import pytest
import tomlkit
from fplinux_cli import common
from fplinux_cli.alpine.sources import aport_sources
from fplinux_cli.build import environment as build_environment
from fplinux_cli.environment import definition, runtime
from fplinux_cli.environment import setup as env_setup
from fplinux_cli.environment.state import EnvironmentState, publish_environment_state


@pytest.fixture
def recipe_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Copy actual causal resources while keeping unrelated project data out of the fixture."""
    source = common.ROOT
    for relative in (
        *definition.ENVIRONMENT_RECIPE_INPUTS,
        "sources.lock.toml",
        "alpine/aports/fplinux-ui/APKBUILD",
        "alpine/aports/fplinux-icons/APKBUILD",
    ):
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


def test_setup_reloads_original_checksums_before_publishing(
    recipe_checkout: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A change to the source catalog after capture prevents publishing the old environment."""
    catalog_path = recipe_checkout / "sources.lock.toml"

    def stage_inputs(context: Path, **_kwargs: object) -> None:
        lock = definition.load_environment_lock()
        (context / "environment-inputs.json").write_bytes(common.canonical_json_bytes(lock))

    def change_original(*_args: object, **_kwargs: object) -> None:
        catalog = tomlkit.parse(catalog_path.read_text())
        llvm = catalog["llvm_project"]
        assert isinstance(llvm, tomlkit.items.Table)
        llvm["checksum"] = "a" * 64
        catalog_path.write_text(tomlkit.dumps(catalog))

    monkeypatch.setattr(env_setup, "ROOT", recipe_checkout)
    monkeypatch.setattr(env_setup, "install_kern", mock.Mock(return_value="kern"))
    monkeypatch.setattr(env_setup, "current_environment_state", mock.Mock(return_value=None))
    monkeypatch.setattr(env_setup, "_stage_environment_inputs", stage_inputs)
    monkeypatch.setattr(env_setup, "_prepare_toolchain", mock.Mock())
    monkeypatch.setattr(env_setup, "_prepare_rootfs", change_original)
    monkeypatch.setattr(env_setup, "remove_environment_rootfs", mock.Mock())
    probe = mock.Mock()
    monkeypatch.setattr(env_setup, "probe_environment_state", probe)

    with pytest.raises(SystemExit, match="environment inputs changed while setup was running"):
        env_setup.setup(reporter=mock.MagicMock(), cache=recipe_checkout / "test-data")
    probe.assert_not_called()


def test_ready_environment_misses_after_changing_source_date_epoch(
    recipe_checkout: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The packaging epoch must match before reusing an installed environment."""
    cache = recipe_checkout / "test-data"
    monkeypatch.setattr(build_environment, "SOURCE_DATE_EPOCH", "1800000000")
    recipe = definition.environment_recipe_digest()
    ready = EnvironmentState(recipe, "a" * 64, "b" * 64)
    publish_environment_state(cache, ready)
    monkeypatch.setattr(runtime, "probe_environment_state", mock.Mock(return_value=ready))
    assert runtime.current_environment_state("kern", recipe, cache=cache) == ready

    monkeypatch.setattr(build_environment, "SOURCE_DATE_EPOCH", "1800000001")
    changed_recipe = definition.environment_recipe_digest()
    assert runtime.current_environment_state("kern", changed_recipe, cache=cache) is None


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
    terminal_source = target_aport.with_name("terminal-render.c")
    terminal_source.write_text("target-only terminal rendering edit\n")
    assert definition.environment_recipe_digest(lock) == original
    host_patch = recipe_checkout / "alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch"
    host_patch.write_bytes(host_patch.read_bytes() + b"\n")
    assert definition.environment_recipe_digest(lock) != original


@pytest.mark.parametrize("change", ["url", "checksum", "build"], ids=("url", "checksum", "build"))
def test_recipe_tracks_terminal_font_archive_and_package_build(
    recipe_checkout: Path, change: str
) -> None:
    """Changing admitted font bytes or their transformation invalidates installed fonts."""
    aport = recipe_checkout / "alpine/aports/fplinux-font-terminus/APKBUILD"
    source = aport.read_text()
    original = definition.environment_recipe_digest()
    if change in {"url", "checksum"}:
        catalog_path = recipe_checkout / "sources.lock.toml"
        catalog = tomlkit.parse(catalog_path.read_text())
        original_source = aport_sources(recipe_checkout, "fplinux-font-terminus")[0]
        record = catalog[original_source.id]
        assert isinstance(record, tomlkit.items.Table)
        record[change] = (
            "https://example.invalid/terminus.tar.gz" if change == "url" else "0" * 128
        )
        catalog_path.write_text(tomlkit.dumps(catalog))
    else:
        aport.write_text(source.replace("alt/dv1.diff", "alt/dv2.diff"))

    assert definition.environment_recipe_digest() != original


def test_recipe_tracks_icon_archive_but_not_glyph_definitions(
    recipe_checkout: Path,
) -> None:
    """Pinned source changes require preparation; rendering uses checkout definitions."""
    original = definition.environment_recipe_digest()
    icons = recipe_checkout / "alpine/aports/fplinux-icons/icons.json"
    icons.write_text('{"glyphs": {}}\n')
    assert definition.environment_recipe_digest() == original
    catalog_path = recipe_checkout / "sources.lock.toml"
    catalog = tomlkit.parse(catalog_path.read_text())
    source = aport_sources(recipe_checkout, "fplinux-icons")[0]
    record = catalog[source.id]
    assert isinstance(record, tomlkit.items.Table)
    record["checksum"] = "0" * 128
    catalog_path.write_text(tomlkit.dumps(catalog))
    assert definition.environment_recipe_digest() != original


def test_recipe_tracks_selected_ui_font_urls_and_checksums_without_local_package_inputs(
    recipe_checkout: Path,
) -> None:
    """Font admissions change the environment while target-only package edits remain hits."""
    aport = recipe_checkout / "alpine/aports/fplinux-ui/APKBUILD"
    source = 'source="@source:face@ local.c"\n' + f'sha512sums="{"2" * 128}  local.c"\n'
    catalog = recipe_checkout / "sources.lock.toml"
    with catalog.open("a") as stream:
        stream.write(
            '\n[face]\nversion="1"\nname="face.ttf"\nurl="https://example.invalid/face.ttf"\n'
            f'algorithm="sha512"\nchecksum="{"1" * 128}"\n'
        )
    original_catalog = catalog.read_text()
    aport.write_text(source)
    original = definition.environment_recipe_digest()
    catalog.write_text(original_catalog.replace("/face.ttf", "/replacement.ttf"))
    assert definition.environment_recipe_digest() != original
    catalog.write_text(original_catalog.replace("1" * 128, "3" * 128))
    assert definition.environment_recipe_digest() != original
    catalog.write_text(original_catalog)
    aport.write_text(source.replace("2" * 128, "4" * 128))
    assert definition.environment_recipe_digest() == original
