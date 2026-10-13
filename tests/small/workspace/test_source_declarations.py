# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks of captured source declarations and workspace recipes."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build.inputs import selected_build_sources
from fplinux_cli.workspace import build_inputs, staging

from tests.small.workspace.workspace_fixtures import (
    empty_package_graph,
    workspace_root,
    write_registration_source,
    write_source_lock_files,
)

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

    from fplinux_cli.workspace.capture import WorkspaceSnapshot


@dataclass(frozen=True)
class SourceDeclarationCheckout:
    """A temporary checkout with two mapped targets and one FDL-only target."""

    root: Path

    def write_source_locks(self) -> None:
        """Create the raw declarations and package metadata owned by this fixture."""
        write_source_lock_files(self.root, "phone-a")
        write_registration_source(self.root)

    def snapshot(self, target: str = "phone-a") -> WorkspaceSnapshot:
        """Capture the real target workspace with controlled configuration discovery."""
        return build_inputs.target_workspace_snapshot(target)


@pytest.fixture
def declaration_checkout(tmp_path: Path) -> Iterator[SourceDeclarationCheckout]:
    """Control config/package discovery while exercising real capture, selection and staging."""
    root = tmp_path
    checkout = SourceDeclarationCheckout(root)
    checkout.write_source_locks()
    sources = root / "sources.lock.toml"
    sources.write_text(
        sources.read_text(encoding="utf-8").replace(
            "[unused]\n",
            f'[host.files]\nloader_c_sha256 = "{"a" * 64}"\n[unused]\n',
        ),
        encoding="utf-8",
    )

    def write(relative: str, contents: str = "source\n") -> None:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")

    target_config: dict[str, Any] = {
        "platform": "demo",
        "build_type": "release",
        "device_data": {"groups": {}},
        "display": {"source": None},
        "linux": {
            "root": {"kind": "initramfs"},
            "config_fragment": "kernel/config.fragment",
            "patches": [],
            "copies": [],
            "appends": [],
        },
        "bootstrap": {"source": "bootstrap"},
        "uboot": {"kind": "none"},
        "fit": {"kind": "none"},
        "image": {"kind": "none"},
    }
    platform_config: dict[str, Any] = {
        "linux": {
            "source_lock": "linux",
            "defconfig": "platforms/demo/kernel/defconfig",
            "build_types": {"release": "platforms/demo/kernel/release.config"},
            "patches": [],
            "copies": [],
            "appends": [],
        },
        "bootstrap": {"vendor_source_lock": "bootstrap", "shared_copies": [], "patches": []},
        "host": {
            "tools": [{"type": "make-archive", "source_lock": "host", "copies": [], "patches": []}]
        },
    }
    for target, pinmap_digest, keymap_digest in (
        ("phone-a", "a" * 64, "b" * 64),
        ("phone-b", "c" * 64, "d" * 64),
        ("phone-fdl", None, None),
    ):
        manifest = 'platform = "demo"\n'
        if pinmap_digest is not None:
            manifest += (
                "[loader_maps]\n"
                f'pinmap = {{member = "{target}/pinmap.bin", sha256 = "{pinmap_digest}"}}\n'
                f'keymap = {{member = "{target}/keymap.bin", sha256 = "{keymap_digest}"}}\n'
            )
        write(f"targets/{target}/target.toml", manifest)
        write(f"targets/{target}/release/manifest.toml")
        write(f"targets/{target}/kernel/config.fragment")
        write(f"targets/{target}/bootstrap/main.c")

    for relative in (
        "platforms/demo/kernel/defconfig",
        "platforms/demo/kernel/release.config",
        "profiles/default/profile.toml",
        "alpine/ramroot-init.sh",
        "common/run.py",
        "common/loader_events.py",
        "platforms/demo/host/adapter.py",
        *selected_build_sources(target_config),
    ):
        write(relative)

    # Build implementation content is irrelevant to these controlled source-input cases.
    # Peer Linux discovery and package selection are separate component boundaries.
    with (
        workspace_root(root),
        empty_package_graph(),
        mock.patch.object(common, "ROOT", root),
        mock.patch.object(build_inputs, "STAGED_BUILD_SOURCES", ("sources.lock.toml",)),
        mock.patch.object(build_inputs, "load_target", return_value=target_config),
        mock.patch.object(build_inputs, "load_platform", return_value=platform_config),
        mock.patch.object(build_inputs, "discover_linux_targets", return_value=()),
    ):
        yield checkout


@pytest.mark.parametrize(
    ("old", "new"),
    [
        ("1" * 64, "7" * 64),
        ("2" * 64, "8" * 64),
        ("a" * 64, "b" * 64),
        (
            'archive_url = "https://example.invalid/host.tar.gz"',
            'archive_url = "https://example.invalid/other-host.tar.gz"',
        ),
    ],
    ids=("linux-digest", "bootstrap-digest", "host-member-digest", "host-url"),
)
def test_selected_global_record_changes_build_recipe(
    declaration_checkout: SourceDeclarationCheckout, old: str, new: str
) -> None:
    """Source digests, a nested host member pin and host URL are causal fields."""
    before = declaration_checkout.snapshot()
    lock = declaration_checkout.root / "sources.lock.toml"
    lock.write_text(lock.read_text(encoding="utf-8").replace(old, new), encoding="utf-8")

    after = declaration_checkout.snapshot()

    assert after.recipe != before.recipe


def test_unused_global_record_preserves_build_recipe_but_changes_staged_sources(
    declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """An unselected download changes delivered source bytes without discarding a build hit."""
    before = declaration_checkout.snapshot()
    before_path = staging.stage_workspace_snapshot(before)
    lock = declaration_checkout.root / "sources.lock.toml"
    previous = lock.read_bytes()
    changed = previous.replace(
        b"https://example.invalid/unused.tar.gz", b"https://example.invalid/other-unused.tar.gz"
    )
    lock.write_bytes(changed)

    after = declaration_checkout.snapshot()
    after_path = staging.stage_workspace_snapshot(after)

    assert after.recipe == before.recipe
    assert after.materialization_recipe != before.materialization_recipe
    assert after_path != before_path
    assert (before_path / "sources.lock.toml").read_bytes() == previous
    assert (after_path / "sources.lock.toml").read_bytes() == changed


def test_common_maps_archive_changes_only_mapped_build_recipes(
    declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """An archive pin is causal for mapped targets and auxiliary for FDL-only consumers."""
    targets = ("phone-a", "phone-b", "phone-fdl")
    before = {target: declaration_checkout.snapshot(target) for target in targets}
    lock = declaration_checkout.root / "platforms/demo/loader/assets.lock.toml"
    lock.write_text(lock.read_text(encoding="utf-8").replace("6" * 64, "8" * 64), encoding="utf-8")

    after = {target: declaration_checkout.snapshot(target) for target in targets}

    assert after["phone-a"].recipe != before["phone-a"].recipe
    assert after["phone-b"].recipe != before["phone-b"].recipe
    assert after["phone-fdl"].recipe == before["phone-fdl"].recipe
    assert after["phone-fdl"].materialization_recipe != before["phone-fdl"].materialization_recipe


@pytest.mark.parametrize(
    ("old", "new"),
    [("phone-a/pinmap.bin", "phone-a/other-pinmap.bin"), ("a" * 64, "e" * 64)],
    ids=("member", "member-digest"),
)
def test_target_map_selection_changes_only_its_build_recipe(
    declaration_checkout: SourceDeclarationCheckout, old: str, new: str
) -> None:
    """One model's extracted member and pin leave another model's build reusable."""
    before_a = declaration_checkout.snapshot("phone-a")
    before_b = declaration_checkout.snapshot("phone-b")
    target = declaration_checkout.root / "targets/phone-a/target.toml"
    target.write_text(target.read_text(encoding="utf-8").replace(old, new), encoding="utf-8")

    assert declaration_checkout.snapshot("phone-a").recipe != before_a.recipe
    assert declaration_checkout.snapshot("phone-b").recipe == before_b.recipe


def test_common_fdl1_pin_changes_every_consumer_build_recipe(
    declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """The common loader file remains a causal input for mapped and FDL-only targets."""
    targets = ("phone-a", "phone-b", "phone-fdl")
    before = {target: declaration_checkout.snapshot(target) for target in targets}
    lock = declaration_checkout.root / "platforms/demo/loader/assets.lock.toml"
    lock.write_text(lock.read_text(encoding="utf-8").replace("5" * 64, "9" * 64), encoding="utf-8")

    for target in targets:
        assert declaration_checkout.snapshot(target).recipe != before[target].recipe


@pytest.mark.parametrize(
    "relative", ["sources.lock.toml", "platforms/demo/loader/assets.lock.toml"]
)
def test_source_declaration_mode_is_causal(
    declaration_checkout: SourceDeclarationCheckout, relative: str
) -> None:
    """A shared declaration's permissions remain causal when its raw contents are auxiliary."""
    lock = declaration_checkout.root / relative
    lock.chmod(0o640)
    before = declaration_checkout.snapshot("phone-fdl")
    lock.chmod(0o600)
    after = declaration_checkout.snapshot("phone-fdl")

    assert after.recipe != before.recipe
    assert after.materialization_recipe != before.materialization_recipe


def test_staging_uses_captured_declaration_bytes_and_modes_after_live_edits(
    declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """Materialization reads the captured raw declarations after the checkout changes."""
    root = declaration_checkout.root
    sources = root / "sources.lock.toml"
    assets = root / "platforms/demo/loader/assets.lock.toml"
    sources.chmod(0o640)
    assets.chmod(0o600)
    captured_sources = sources.read_bytes()
    captured_assets = assets.read_bytes()
    snapshot = declaration_checkout.snapshot()

    sources.write_bytes(captured_sources.replace(b"host.tar.gz", b"live-host.tar.gz"))
    sources.chmod(0o644)
    assets.write_bytes(captured_assets.replace(b"fdl1.bin", b"live-fdl1.bin"))
    assets.chmod(0o644)
    workspace = staging.stage_workspace_snapshot(snapshot)

    staged_sources = workspace / "sources.lock.toml"
    staged_assets = workspace / "platforms/demo/loader/assets.lock.toml"
    assert staged_sources.read_bytes() == captured_sources
    assert staged_sources.stat().st_mode & 0o777 == 0o640
    assert staged_assets.read_bytes() == captured_assets
    assert staged_assets.stat().st_mode & 0o777 == 0o600
