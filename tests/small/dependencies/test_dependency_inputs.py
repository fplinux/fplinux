# SPDX-License-Identifier: GPL-2.0-only
"""Exact originals selected through current package and environment consumers."""

from __future__ import annotations

import base64
import hashlib
import json
import shutil
from dataclasses import asdict, dataclass
from pathlib import Path

import pytest
import tomlkit
from fplinux_cli.alpine import registration
from fplinux_cli.dependencies.inputs import (
    DependencyInput,
    acquire_inputs,
    build_source_inputs,
    dependency_selection,
    environment_inputs,
    npm_declarations,
)
from fplinux_cli.dependencies.snapshots import require_checkout

_FIXTURES = Path(__file__).parents[2] / "fixtures/dependencies"


@dataclass(frozen=True)
class DependencyCheckout:
    """Own one synthetic checkout and its controlled current declarations."""

    root: Path

    def write(self, name: str, text: str) -> None:
        """Write a declaration below the test-owned checkout."""
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def source(self, name: str, *, filename: str, checksum: str) -> None:
        """Append one independently pinned original to the fixture's source catalog."""
        path = self.root / "sources.lock.toml"
        with path.open("a") as stream:
            stream.write(
                f'\n[{name}]\nversion = "1.2.3"\nname = "{filename}"\n'
                f'url = "https://example.invalid/{filename}"\nalgorithm = "sha512"\n'
                f'checksum = "{checksum}"\n'
            )


@pytest.fixture
def dependency_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> DependencyCheckout:
    """Supply package selection and pinned originals without external state."""
    monkeypatch.setattr(registration, "COMMON_PACKAGES", ("fplinux-base",))
    monkeypatch.setattr(
        registration,
        "LOCAL_BUILD_DEPENDENCIES",
        {"fplinux-bash": ("fplinux-ncurses", "fplinux-readline")},
    )
    shutil.copytree(_FIXTURES / "checkout", tmp_path, dirs_exist_ok=True)
    (tmp_path / "sources.lock.toml.fixture").rename(tmp_path / "sources.lock.toml")
    checkout = DependencyCheckout(tmp_path)
    checkout.write(
        "package-lock.json",
        json.dumps(
            {
                "packages": {
                    "": {"devDependencies": {"tool": "3.0.0"}},
                    "node_modules/tool": {
                        "version": "3.0.0",
                        "resolved": "https://example.invalid/tool.tgz",
                        "integrity": "sha512-"
                        + base64.b64encode(bytes.fromhex("11" * 64)).decode(),
                    },
                }
            }
        ),
    )
    for package in (
        "fplinux-base",
        "fplinux-bash",
        "fplinux-ncurses",
        "fplinux-readline",
        "fplinux-libtsm",
        "fplinux-ui",
        "fplinux-font-terminus",
        "fplinux-icons",
        "fplinux-debug-tools",
    ):
        checkout.write(
            f"alpine/aports/{package}/APKBUILD",
            f'pkgname={package}\npkgver=1\npkgrel=0\narch="armv7"\n',
        )
    return checkout


def test_host_original_has_one_checksum_size_and_cache_slot(
    dependency_checkout: DependencyCheckout,
) -> None:
    inputs, _ = dependency_selection(dependency_checkout.root)
    records = {item.key: item for item in inputs}
    assert records["source:linux_host_libusb"] == DependencyInput(
        "source:linux_host_libusb",
        "https://example.invalid/linux-libusb.tar.gz",
        "1" * 64,
        None,
        "downloads/sources/linux_host_libusb/linux-libusb.tar.gz",
        "source-original",
    )
    assert records["source:windows_host_llvm_mingw"].size == 123
    assert sum(item.url == "https://example.invalid/toolchain.tar.xz" for item in inputs) == 1
    assert records["source:windows_host_python"].size == 456
    assert records["source:windows_driver_libwdi"].sha256 == "4" * 64
    assert all(
        not item.key.startswith(("source:windows_", "source:linux_host_"))
        for item in environment_inputs(dependency_checkout.root)
    )


def test_runtime_source_selection_keeps_report_original_restorable_offline(
    dependency_checkout: DependencyCheckout,
) -> None:
    """Runtime source companions omit report originals while full snapshots preserve them."""
    checkout = dependency_checkout
    contents = b"report source original\n"
    checkout.source(
        "board_report", filename="report.tar.gz", checksum=hashlib.sha512(contents).hexdigest()
    )
    path = checkout.root / "platforms/test-platform/platform.toml"
    with path.open("a") as manifest:
        manifest.write(
            '\n[host.runtime_tools]\nloader = "loader"\n'
            '\n[[host.tools]]\ntype = "make-archive"\nname = "report"\n'
            'source_lock = "board_report"\narchive_prefix = "report/"\nmembers = []\n'
        )
    platform = tomlkit.parse(path.read_text()).unwrap()
    config = {
        "rootfs": {"base_packages": [], "packages": []},
        "bundle": {"packages": []},
        "linux": {"root": {"kind": "initramfs"}},
        "device_data": {"groups": {}},
        "uboot": {"kind": "none"},
        "loader_assets": [],
    }

    runtime_inputs = build_source_inputs(checkout.root, platform, config)
    complete_inputs, _context = dependency_selection(checkout.root)
    report = next(item for item in complete_inputs if item.key == "source:board_report")
    cache = checkout.root / ".cache"
    original = cache / report.destination
    original.parent.mkdir(parents=True)
    original.write_bytes(contents)
    restored = acquire_inputs([report], cache, offline=True)

    assert all(item.key != "source:board_report" for item in runtime_inputs)
    assert restored["source:board_report"].read_bytes() == contents


@pytest.mark.parametrize(
    "source_id", ["linux_host_libusb", "windows_host_llvm_mingw", "windows_driver_libwdi"]
)
def test_host_original_changes_snapshot_without_changing_environment(
    dependency_checkout: DependencyCheckout,
    source_id: str,
) -> None:
    environment = environment_inputs(dependency_checkout.root)
    inputs, context = dependency_selection(dependency_checkout.root)
    saved = {"inputs": [{"input": asdict(item)} for item in inputs], "context": context}
    path = dependency_checkout.root / "sources.lock.toml"
    document = tomlkit.parse(path.read_text())
    record = document[source_id]
    assert isinstance(record, tomlkit.items.Table)
    record["checksum"] = "9" * 64
    path.write_text(tomlkit.dumps(document))
    current_inputs, current_context = dependency_selection(dependency_checkout.root)
    with pytest.raises(SystemExit, match="does not match this checkout"):
        require_checkout(saved, current_inputs, current_context)
    assert environment_inputs(dependency_checkout.root) == environment


@pytest.mark.parametrize(
    ("package", "source_id", "filename"),
    [
        ("fplinux-ui", "face", "face.ttf"),
        ("fplinux-font-terminus", "terminus", "terminus.tar.gz"),
        ("fplinux-icons", "tabler", "tabler.tgz"),
    ],
)
def test_environment_and_package_share_one_native_original(
    dependency_checkout: DependencyCheckout,
    package: str,
    source_id: str,
    filename: str,
) -> None:
    dependency_checkout.source(source_id, filename=filename, checksum="8" * 128)
    dependency_checkout.write(
        f"alpine/aports/{package}/APKBUILD",
        f'pkgname={package}\npkgver=1\nsource="@source:{source_id}@"\n',
    )
    environment = environment_inputs(dependency_checkout.root)
    original = next(item for item in environment if item.key == f"source:{source_id}")
    assert original.algorithm == "sha512"
    assert original.checksum == "8" * 128
    assert original.sha256 is None
    assert original.destination == f"downloads/sources/{source_id}/{filename}"
    inputs, context = dependency_selection(dependency_checkout.root)
    assert sum(item.key == original.key for item in inputs) == 1
    producers = context["producers"]
    assert isinstance(producers, list)
    assert package not in producers
    dependency_checkout.write(
        "targets/test-phone/target.toml",
        f'platform="test-platform"\n[rootfs]\npackages=["{package}"]\n[bundle]\npackages=[]\n',
    )
    selected, context = dependency_selection(dependency_checkout.root)
    selected_producers = context["producers"]
    assert isinstance(selected_producers, list)
    assert package in selected_producers
    assert sum(item.key == original.key for item in selected) == 1


def test_debug_and_bundle_selection_retains_its_originals(
    dependency_checkout: DependencyCheckout,
) -> None:
    dependency_checkout.source("perf", filename="perf.tar.xz", checksum="7" * 128)
    dependency_checkout.source("bundle", filename="bundle.tar.gz", checksum="6" * 128)
    dependency_checkout.write(
        "alpine/aports/fplinux-debug-tools/APKBUILD",
        'pkgname=fplinux-debug-tools\nsource="@source:perf@"\n',
    )
    dependency_checkout.write(
        "alpine/aports/fplinux-bundle/APKBUILD",
        'pkgname=fplinux-bundle\nsource="@source:bundle@"\n',
    )
    target = dependency_checkout.root / "targets/test-phone/target.toml"
    target.write_text(
        target.read_text().replace(
            "[bundle]\npackages = []",
            '[bundle]\npackages = ["fplinux-bundle"]',
        )
    )
    inputs, context = dependency_selection(dependency_checkout.root)
    assert {"source:perf", "source:bundle"} <= {item.key for item in inputs}
    producers = context["producers"]
    assert isinstance(producers, list)
    assert "fplinux-debug-tools" in producers
    assert "fplinux-bundle" in producers


def test_quality_projection_tracks_original_and_selected_member_pins(
    dependency_checkout: DependencyCheckout,
) -> None:
    original = next(
        item
        for item in environment_inputs(dependency_checkout.root)
        if item.key == "source:spreadtrum_flash"
    )
    assert original.url == "https://example.invalid/spreadtrum-flash.tar.gz"
    assert original.sha256 == "a" * 64
    inputs, context = dependency_selection(dependency_checkout.root)
    saved = {"inputs": [{"input": asdict(item)} for item in inputs], "context": context}
    path = dependency_checkout.root / "sources.lock.toml"
    path.write_text(
        path.read_text().replace(
            'makefile_sha256 = "' + "b" * 64,
            'makefile_sha256 = "' + "e" * 64,
        )
    )
    current_inputs, current_context = dependency_selection(dependency_checkout.root)
    assert current_inputs == inputs
    with pytest.raises(SystemExit, match="does not match this checkout"):
        require_checkout(saved, current_inputs, current_context)


def test_source_built_loader_inventory_selects_its_three_originals(
    dependency_checkout: DependencyCheckout,
) -> None:
    platform = dependency_checkout.root / "platforms/test-platform/platform.toml"
    platform.write_text(platform.read_text().replace('backend = "ums9117"', 'backend = "sc9820e"'))
    with (dependency_checkout.root / "sources.lock.toml").open("ab") as stream:
        stream.write(
            (
                Path(__file__).parents[2] / "fixtures/sc9820e-manifest/sources.lock.toml"
            ).read_bytes()
        )
    target = dependency_checkout.root / "targets/test-phone/target.toml"
    with target.open("a") as stream:
        stream.write(
            '\n[loader.build]\nfdl1_source="chipram"\nfdl2_source="u_boot"\nsml_source="trusted_firmware"\n'
        )
    inputs, context = dependency_selection(dependency_checkout.root)
    records = {item.key: item for item in inputs}
    assert records["source:chipram"].url == "https://example.invalid/chipram.tar.gz"
    assert records["source:u_boot"].sha256 == "e" * 64
    assert (
        records["source:trusted_firmware"].destination
        == "downloads/sources/trusted_firmware/trusted_firmware.tar.gz"
    )
    assert "source:loader" not in records
    assets = context["assets"]
    assert isinstance(assets, dict)
    assert {record["role"] for record in assets["test-phone"]} == {"fdl1", "fdl2", "sml"}


def test_unrelated_source_and_presentation_preserve_selection(
    dependency_checkout: DependencyCheckout,
) -> None:
    inputs, before = dependency_selection(dependency_checkout.root)
    dependency_checkout.source("unused", filename="unused.tar.gz", checksum="9" * 128)
    path = dependency_checkout.root / "alpine/aports/fplinux-base/APKBUILD"
    path.write_text("# Maintainer comment\n" + path.read_text())
    dependency_checkout.write("platforms/test-platform/linux/driver.c", "int changed;\n")
    current_inputs, after = dependency_selection(dependency_checkout.root)
    assert current_inputs == inputs
    assert after == before


def test_provider_and_local_source_order_remain_semantic_inputs(
    dependency_checkout: DependencyCheckout,
) -> None:
    path = dependency_checkout.root / "alpine/aports/fplinux-base/APKBUILD"
    path.write_text(path.read_text() + 'provides="virtual=1"\nsource="first.patch second.patch"\n')
    _, before = dependency_selection(dependency_checkout.root)
    path.write_text(path.read_text().replace("virtual=1", "virtual=2"))
    _, after = dependency_selection(dependency_checkout.root)
    assert before != after
    path.write_text(
        path.read_text().replace("first.patch second.patch", "second.patch first.patch")
    )
    _, reordered = dependency_selection(dependency_checkout.root)
    assert reordered != after


def test_environment_inventory_does_not_require_targets(
    dependency_checkout: DependencyCheckout,
) -> None:
    (dependency_checkout.root / "targets/test-phone/target.toml").unlink()
    records = {item.key: item for item in environment_inputs(dependency_checkout.root)}
    assert records["environment:tool"].size == 12
    assert records["source:tool_source"].url == "https://example.invalid/tool-2.0.tar.gz"
    assert records["npm:node_modules/tool"].checksum == "11" * 64


@pytest.mark.parametrize(
    "source",
    [
        "https://example.invalid/unpinned.tar.gz",
        "$(printf https://example.invalid/source)",
        "@source:missing@",
    ],
)
def test_unowned_external_source_cannot_claim_complete_inventory(
    dependency_checkout: DependencyCheckout,
    source: str,
) -> None:
    dependency_checkout.write(
        "alpine/aports/fplinux-libtsm/APKBUILD",
        f'pkgname=fplinux-libtsm\nsource="{source}"\n',
    )
    with pytest.raises(
        SystemExit,
        match=r"source references|command substitution|source lock entry is missing",
    ):
        dependency_selection(dependency_checkout.root)


def test_bundled_npm_inputs_use_the_exact_parent_archive() -> None:
    context, inputs = npm_declarations(
        _FIXTURES / "bundled-package-lock.json",
        key_prefix="npm",
        cache_directory="downloads/npm",
        purpose="npm-package",
    )
    assert len(inputs) == 1
    assert inputs[0].key == "npm:node_modules/tool"
    assert inputs[0].url == "https://example.invalid/tool-1.0.0.tgz"
    assert inputs[0].sha256 == "11" * 32
    packages = context["packages"]
    assert isinstance(packages, dict)
    assert packages["node_modules/tool/node_modules/parser"] == {
        "version": "2.0.0",
        "inBundle": True,
        "dependencies": {"helper": "3.0.0"},
    }
    assert packages["node_modules/tool/node_modules/parser/node_modules/helper"] == {
        "version": "3.0.0",
        "inBundle": True,
    }


@pytest.mark.parametrize("bundled", [False, True], ids=["missing-pin", "orphan-bundle"])
def test_unpinned_npm_package_has_no_saved_original(tmp_path: Path, *, bundled: bool) -> None:
    path = tmp_path / "package-lock.json"
    path.write_text(
        json.dumps(
            {
                "packages": {"node_modules/other": {"version": "1", "inBundle": bundled}},
            }
        )
    )
    with pytest.raises(SystemExit, match=r"no exact .*registry input"):
        npm_declarations(
            path,
            key_prefix="npm",
            cache_directory="downloads/npm",
            purpose="npm-package",
        )


def test_input_record_rejects_unpinned_bytes_and_escaping_destinations() -> None:
    with pytest.raises(SystemExit, match="exact SHA-256 or SHA-512"):
        DependencyInput(
            "test", "https://example.invalid/file", None, None, "downloads/file", "test"
        )
    with pytest.raises(SystemExit, match="normalized relative path"):
        DependencyInput(
            "test",
            "https://example.invalid/file",
            "a" * 64,
            None,
            "downloads/../file",
            "test",
        )
