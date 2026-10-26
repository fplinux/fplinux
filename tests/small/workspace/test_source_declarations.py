# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks of captured source declarations and workspace recipes."""

from __future__ import annotations

from dataclasses import dataclass, replace
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build.inputs import selected_build_sources
from fplinux_cli.build.runtime import runtime_source_files
from fplinux_cli.device_data.inputs import capture_snapshot_device_data
from fplinux_cli.workspace import build_inputs, staging
from fplinux_cli.workspace.capture import snapshot_from_inventory

from tests import ROOT
from tests.small.device_data.firmware_fixtures import (
    qin_firmware as qin_firmware,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.small.workspace.workspace_fixtures import (
    empty_package_graph,
    workspace_root,
    write_registration_source,
    write_source_lock_files,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator
    from pathlib import Path

    from fplinux_cli.workspace.capture import WorkspaceSnapshot

    from tests.small.device_data.firmware_fixtures import QinFirmwareContext


@dataclass(frozen=True)
class SourceDeclarationCheckout:
    """A temporary checkout with three targets sharing one pinned loader."""

    root: Path
    target_config: dict[str, Any]
    platform_config: dict[str, Any]

    def snapshot(
        self, target: str = "phone-a", *, include_device_data: bool = True
    ) -> WorkspaceSnapshot:
        """Capture the real target workspace with controlled configuration discovery."""
        return build_inputs.target_workspace_snapshot(
            target, include_device_data=include_device_data
        )


@pytest.fixture
def declaration_checkout(tmp_path: Path) -> Iterator[SourceDeclarationCheckout]:
    """Control config/package discovery while exercising real capture, selection and staging."""
    root = tmp_path
    write_source_lock_files(root, "phone-a")
    write_registration_source(root)
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
        "bootstrap": {"backend": "ums9117", "source": "bootstrap"},
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
        "bootstrap": {
            "backend": "ums9117",
            "vendor_source_lock": "bootstrap",
            "shared_copies": [],
            "patches": [],
        },
        "host": {
            "runtime_tools": {"loader": "loader"},
            "tools": [
                {
                    "name": "loader",
                    "type": "make-archive",
                    "source_lock": "host",
                    "copies": [],
                    "patches": [],
                }
            ],
        },
    }
    checkout = SourceDeclarationCheckout(root, target_config, platform_config)
    for target in ("phone-a", "phone-b", "phone-fdl"):
        write(f"targets/{target}/target.toml", 'platform = "demo"\n')
        write(f"targets/{target}/release/manifest.toml")
        write(f"targets/{target}/kernel/config.fragment")
        write(f"targets/{target}/bootstrap/main.c")

    for relative in (
        "platforms/demo/kernel/defconfig",
        "platforms/demo/kernel/release.config",
        "profiles/default/profile.toml",
        "alpine/ramroot-init.sh",
        *selected_build_sources(target_config),
    ):
        write(relative)
    for source in runtime_source_files("phone-a", target_config, root=root).values():
        write(source.relative_to(root).as_posix())

    # Build implementation content is irrelevant to these controlled source-input cases.
    # Peer Linux discovery and package selection are separate component boundaries.
    with (
        workspace_root(root),
        empty_package_graph(),
        mock.patch.object(common, "ROOT", root),
        mock.patch.object(
            build_inputs, "STAGED_BUILD_SOURCES", ("sources.lock.toml", "alpine.lock.toml")
        ),
        mock.patch.object(build_inputs, "load_target", return_value=target_config),
        mock.patch.object(build_inputs, "load_platform", return_value=platform_config),
        mock.patch.object(build_inputs, "discover_linux_targets", return_value=()),
    ):
        yield checkout


@pytest.fixture
def sc_declaration_checkout(
    declaration_checkout: SourceDeclarationCheckout,
) -> SourceDeclarationCheckout:
    """Select source-built loaders with target patches and three pinned source archives."""
    root = declaration_checkout.root
    target = declaration_checkout.target_config
    target["bootstrap"]["backend"] = "sc9820e"
    target["loader"] = {
        "build": {
            "source": "loader",
            "fdl1_source": "chipram",
            "fdl2_source": "u_boot",
            "sml_source": "trusted_firmware",
        }
    }
    platform = declaration_checkout.platform_config
    platform["bootstrap"] = {"backend": "sc9820e", "toolchain": "arm-none-eabi"}
    manifest = root / "platforms/demo/platform.toml"
    manifest.write_text(
        manifest.read_text(encoding="utf-8")
        .replace('backend = "ums9117"', 'backend = "sc9820e"')
        .replace('vendor_source_lock = "bootstrap"\n', ""),
        encoding="utf-8",
    )
    (root / "targets/phone-a/target.toml").write_text(
        (ROOT / "tests/fixtures/sc9820e-manifest/target.toml")
        .read_text(encoding="utf-8")
        .replace('platform = "sc9820e"', 'platform = "demo"'),
        encoding="utf-8",
    )
    (root / "platforms/demo/loader/assets.lock.toml").unlink()
    entry = root / "platforms/demo/bootstrap/entry.s"
    entry.parent.mkdir(parents=True)
    entry.write_bytes(b"source-built entry\n")
    loader_patch = root / "targets/phone-a/loader/chipram.patch"
    loader_patch.parent.mkdir()
    loader_patch.write_bytes(b"board-owned loader patch\n")
    with (root / "sources.lock.toml").open("ab") as lock:
        lock.write(b"\n")
        lock.write((ROOT / "tests/fixtures/sc9820e-manifest/sources.lock.toml").read_bytes())
    for relative in selected_build_sources(target):
        source = root / relative
        if not source.exists():
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b"build implementation fixture\n")
    return declaration_checkout


def test_sc_snapshot_stages_loader_sources_without_unrelated_cache_bytes(
    sc_declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """A RAM build captures its entry and loader source while excluding unrelated local bytes."""
    checkout = sc_declaration_checkout
    before = checkout.snapshot()
    local_loader = checkout.root / ".cache/loader-inputs/phone-a/fdl1.bin"
    local_loader.parent.mkdir(parents=True)
    local_loader.write_bytes(b"private-loader-original")

    after = checkout.snapshot()
    workspace = staging.stage_workspace_snapshot(after)

    assert after.recipe == before.recipe
    assert (workspace / "platforms/demo/bootstrap/entry.s").read_bytes() == b"source-built entry\n"
    assert (workspace / "targets/phone-a/loader/chipram.patch").read_bytes() == (
        b"board-owned loader patch\n"
    )
    assert b'fdl1_source = "chipram"' in (workspace / "targets/phone-a/target.toml").read_bytes()
    assert not (workspace / "platforms/demo/loader/assets.lock.toml").exists()
    assert not (workspace / ".fplinux-inputs/loader").exists()
    assert all(
        b"private-loader-original" not in source.contents for source in after.materialized_files
    )


@pytest.mark.parametrize(
    ("relative", "original", "changed"),
    [
        ("platforms/demo/bootstrap/entry.s", b"source-built entry\n", b"changed entry\n"),
        (
            "targets/phone-a/loader/chipram.patch",
            b"board-owned loader patch\n",
            b"changed loader patch\n",
        ),
    ],
    ids=("entry", "loader-patch"),
)
@pytest.mark.parametrize(
    "include_device_data", [True, False], ids=["ordinary", "without-device-data"]
)
def test_sc_source_change_misses_while_staging_keeps_captured_bytes(
    sc_declaration_checkout: SourceDeclarationCheckout,
    relative: str,
    original: bytes,
    changed: bytes,
    *,
    include_device_data: bool,
) -> None:
    """The handoff and loader sources are causal and retain snapshot ownership."""
    checkout = sc_declaration_checkout
    before = checkout.snapshot(include_device_data=include_device_data)
    (checkout.root / relative).write_bytes(changed)
    after = checkout.snapshot(include_device_data=include_device_data)
    previous_workspace = staging.stage_workspace_snapshot(before)

    assert after.recipe != before.recipe
    assert (previous_workspace / relative).read_bytes() == original


@pytest.mark.parametrize("digest", ["d" * 64, "e" * 64, "f" * 64], ids=["fdl1", "fdl2", "sml"])
def test_sc_selected_loader_archive_change_misses_build_recipe(
    sc_declaration_checkout: SourceDeclarationCheckout, digest: str
) -> None:
    """Every loader stage's actual source archive participates in the selected build."""
    checkout = sc_declaration_checkout
    before = checkout.snapshot()
    lock = checkout.root / "sources.lock.toml"
    lock.write_text(lock.read_text().replace(digest, "7" * 64), encoding="utf-8")

    assert checkout.snapshot().recipe != before.recipe


def test_without_device_data_ignores_prepared_radio_changes_and_stages_no_payload(
    sc_declaration_checkout: SourceDeclarationCheckout, qin_firmware: QinFirmwareContext
) -> None:
    """An external-root build captures private radio unless explicitly built without it."""
    checkout = sc_declaration_checkout
    checkout.target_config["linux"]["root"]["kind"] = "external"
    firmware = replace(qin_firmware, target="phone-a")
    checkout.target_config["device_data"]["groups"] = firmware.groups
    first_generation = firmware.publish()
    ordinary_before = checkout.snapshot()
    clean_before = build_inputs.target_workspace_snapshot("phone-a", include_device_data=False)
    second_generation = firmware.publish(system_calibration=b"system calibration B\x00")

    ordinary_after = checkout.snapshot()
    clean_after = build_inputs.target_workspace_snapshot("phone-a", include_device_data=False)
    clean_workspace = staging.stage_workspace_snapshot(clean_after)
    ordinary_workspace = staging.stage_workspace_snapshot(ordinary_after)

    assert ordinary_before.recipe != ordinary_after.recipe
    assert clean_before.recipe == clean_after.recipe
    assert clean_after.recipe != ordinary_after.recipe
    assert capture_snapshot_device_data("phone-a", firmware.groups, clean_workspace) == {}
    captured = capture_snapshot_device_data("phone-a", firmware.groups, ordinary_workspace)
    assert {item.destination: item.contents for item in captured["wcn-firmware"]}[
        "qin1splus/system-cali.bin"
    ] == b"system calibration B\x00"
    assert first_generation.is_dir()
    assert second_generation.is_dir()


def test_sc_unused_bootstrap_archive_keeps_hit_and_selected_host_archive_misses(
    sc_declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """The source-built backend does not acquire a causal dependency on a UMS-only archive."""
    checkout = sc_declaration_checkout
    before = checkout.snapshot()
    lock = checkout.root / "sources.lock.toml"
    lock.write_text(lock.read_text(encoding="utf-8").replace("2" * 64, "9" * 64), encoding="utf-8")
    unused_changed = checkout.snapshot()
    lock.write_text(lock.read_text(encoding="utf-8").replace("3" * 64, "8" * 64), encoding="utf-8")
    host_changed = checkout.snapshot()

    assert unused_changed.recipe == before.recipe
    assert host_changed.recipe != unused_changed.recipe


@pytest.mark.parametrize(
    ("old", "new"),
    [
        ("1" * 64, "7" * 64),
        ("2" * 64, "8" * 64),
        ("a" * 64, "b" * 64),
        (
            'url = "https://example.invalid/host.tar.gz"',
            'url = "https://example.invalid/other-host.tar.gz"',
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


def test_board_storage_additions_change_the_selected_workspace_recipe(
    declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """Changing a board validation rule cannot reuse the selected build artifact."""
    checkout = declaration_checkout
    manifest = checkout.root / "targets/phone-a/target.toml"
    with manifest.open("a") as declaration:
        declaration.write(
            "\n[linux]\nadditional_forbidden_config = []\nadditional_forbidden_dtb_markers = []\n"
        )
    before = checkout.snapshot()
    manifest.write_text(
        manifest.read_text().replace(
            "additional_forbidden_config = []",
            'additional_forbidden_config = ["CONFIG_BOARD_STORAGE=y"]',
        )
    )

    assert checkout.snapshot().recipe != before.recipe


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


@pytest.fixture
def auxiliary_host_checkout(
    declaration_checkout: SourceDeclarationCheckout,
) -> SourceDeclarationCheckout:
    """Declare report and ECC inputs beside the loader used by this synthetic runtime."""
    checkout = declaration_checkout
    for name in ("report", "ums9117-nand-ecc"):
        patch = f"platforms/demo/host/{name}.patch"
        source = checkout.root / patch
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(b"original tool patch\n")
        checkout.platform_config["host"]["tools"].append(
            {
                "name": name,
                "type": "make-archive",
                "source_lock": "unused",
                "copies": [],
                "patches": [patch],
            }
        )
        with (checkout.root / "platforms/demo/platform.toml").open("a") as manifest:
            manifest.write(
                f'\n[[host.tools]]\nname = "{name}"\ntype = "make-archive"\n'
                f'source_lock = "unused"\ncopies = []\npatches = ["{patch}"]\n'
            )
    return checkout


@pytest.mark.parametrize("tool", ["report", "ums9117-nand-ecc"])
def test_unused_host_tool_patch_keeps_runtime_recipe_but_changes_explicit_tool_snapshot(
    auxiliary_host_checkout: SourceDeclarationCheckout, tool: str
) -> None:
    """An ordinary build stays reusable while an explicit tool captures its own changed bytes."""
    checkout = auxiliary_host_checkout
    before = checkout.snapshot()
    explicit_before = snapshot_from_inventory(
        lambda: build_inputs.target_build_source_files("phone-a", host_tools=(tool,))
    )
    patch = checkout.root / f"platforms/demo/host/{tool}.patch"
    patch.write_bytes(b"changed tool patch\n")

    after = checkout.snapshot()
    explicit_after = snapshot_from_inventory(
        lambda: build_inputs.target_build_source_files("phone-a", host_tools=(tool,))
    )

    assert after.recipe == before.recipe
    assert explicit_after.recipe != explicit_before.recipe
    staged = staging.stage_workspace_snapshot(explicit_after)
    assert (staged / f"platforms/demo/host/{tool}.patch").read_bytes() == b"changed tool patch\n"


def test_selected_runtime_ecc_patch_invalidates_the_build_recipe(
    auxiliary_host_checkout: SourceDeclarationCheckout,
) -> None:
    """A real RAM preparation consumer makes decoder inputs causal again."""
    checkout = auxiliary_host_checkout
    checkout.target_config["nand"] = {"ecc_format": "ums9117-bch12"}
    checkout.target_config["device_data"] = {"groups": {"audio-profile": []}}
    checkout.target_config["audio_policy"] = {}
    sources = runtime_source_files("phone-a", checkout.target_config, root=checkout.root)
    for source in sources.values():
        if not source.exists():
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b"preparation implementation\n")
    before = checkout.snapshot()
    patch = checkout.root / "platforms/demo/host/ums9117-nand-ecc.patch"
    patch.write_bytes(b"changed decoder patch\n")

    assert checkout.snapshot().recipe != before.recipe


def test_minibase_catalog_change_misses_target_recipe_while_unrelated_record_keeps_hit(
    declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """The minirootfs reference binds its resolved original even when the lock is unchanged."""
    checkout = declaration_checkout
    before = checkout.snapshot()
    lock = checkout.root / "sources.lock.toml"
    original = lock.read_text()
    lock.write_text(original.replace("4" * 64, "e" * 64))
    assert checkout.snapshot().recipe == before.recipe
    lock.write_text(original.replace("6" * 64, "b" * 64))
    changed = checkout.snapshot()
    assert changed.recipe != before.recipe
    assert checkout.snapshot().recipe == changed.recipe


@pytest.mark.parametrize("full_boot", [False, True], ids=["ram", "full-uboot"])
def test_uboot_catalog_change_is_causal_only_when_profile_consumes_it(
    declaration_checkout: SourceDeclarationCheckout, *, full_boot: bool
) -> None:
    """A full U-Boot profile follows its original pin without tying RAM boot to that record."""
    checkout = declaration_checkout
    if full_boot:
        checkout.target_config["uboot"] = {
            "kind": "full",
            "source": "platforms/demo/uboot/u-boot.lock.toml",
            "defconfig": "uboot/demo_defconfig",
            "patches": [],
            "copies": [],
        }
        lock = checkout.root / "platforms/demo/uboot/u-boot.lock.toml"
        lock.parent.mkdir()
        lock.write_text('source = "storage_boot"\n')
        defconfig = checkout.root / "targets/phone-a/uboot/demo_defconfig"
        defconfig.parent.mkdir()
        defconfig.write_text("CONFIG_BOARD=y\n")
        implementation = checkout.root / "scripts/fplinux_cli/build/storage/uboot.py"
        implementation.parent.mkdir(parents=True, exist_ok=True)
        implementation.write_text("build implementation fixture\n")
    before = checkout.snapshot()
    sources = checkout.root / "sources.lock.toml"
    original = sources.read_text()
    sources.write_text(original.replace("4" * 64, "e" * 64))
    assert checkout.snapshot().recipe == before.recipe
    sources.write_text(original.replace("7" * 64, "b" * 64))
    changed = checkout.snapshot()
    assert (changed.recipe != before.recipe) == full_boot
    assert checkout.snapshot().recipe == changed.recipe


def test_fdl1_catalog_resolution_uses_captured_bytes_after_live_catalog_edit(
    declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """Resolving an asset does not reread its source catalog after source capture."""
    checkout = declaration_checkout
    baseline = checkout.snapshot()
    lock = checkout.root / "sources.lock.toml"
    original = lock.read_text()

    def capture_then_edit(
        inventory_function: Callable[[], list[tuple[str, Path]]],
    ) -> WorkspaceSnapshot:
        snapshot = snapshot_from_inventory(inventory_function)
        lock.write_text(original.replace("5" * 64, "b" * 64))
        return snapshot

    with mock.patch.object(build_inputs, "snapshot_from_inventory", side_effect=capture_then_edit):
        captured = checkout.snapshot()
    assert captured.recipe == baseline.recipe
    assert checkout.snapshot().recipe != baseline.recipe


def test_target_bootstrap_changes_only_its_build_recipe(
    declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """One board's early initialization leaves another board's build reusable."""
    before_a = declaration_checkout.snapshot("phone-a")
    before_b = declaration_checkout.snapshot("phone-b")
    source = declaration_checkout.root / "targets/phone-a/bootstrap/main.c"
    source.write_text("changed board initialization\n", encoding="utf-8")

    assert declaration_checkout.snapshot("phone-a").recipe != before_a.recipe
    assert declaration_checkout.snapshot("phone-b").recipe == before_b.recipe


def test_common_fdl1_pin_changes_every_consumer_build_recipe(
    declaration_checkout: SourceDeclarationCheckout,
) -> None:
    """The shared first-stage loader is causal for every target that consumes it."""
    targets = ("phone-a", "phone-b", "phone-fdl")
    before = {target: declaration_checkout.snapshot(target) for target in targets}
    lock = declaration_checkout.root / "sources.lock.toml"
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
