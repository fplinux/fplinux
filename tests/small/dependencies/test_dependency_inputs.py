# SPDX-License-Identifier: GPL-2.0-only
"""Host tests for exact dependency declarations and semantic archive context."""

from __future__ import annotations

import base64
import json
import shutil
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
import tomlkit
from fplinux_cli import common
from fplinux_cli.alpine import registration as alpine_registration
from fplinux_cli.cli import dependencies as dependency_cli
from fplinux_cli.dependencies.inputs import (
    DependencyInput,
    dependency_selection,
    environment_inputs,
    npm_declarations,
)
from fplinux_cli.dependencies.snapshots import require_checkout
from tomlkit.items import Table

if TYPE_CHECKING:
    from collections.abc import Iterator


@dataclass(frozen=True)
class DependencyCheckout:
    """Write controlled external declarations in one temporary checkout."""

    root: Path

    def write(self, name: str, text: str) -> None:
        """Write a scenario declaration inside the owned checkout."""
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


_FIXTURES = Path(__file__).parents[2] / "fixtures/dependencies"


@pytest.fixture
def dependency_checkout(monkeypatch: pytest.MonkeyPatch) -> Iterator[DependencyCheckout]:
    """Own the synthetic package graph and its literal external declarations."""
    monkeypatch.setattr(alpine_registration, "COMMON_PACKAGES", ("fplinux-base",))
    monkeypatch.setattr(
        alpine_registration,
        "LOCAL_BUILD_DEPENDENCIES",
        {"fplinux-bash": ("fplinux-ncurses", "fplinux-readline")},
    )
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        shutil.copytree(_FIXTURES / "checkout", root, dirs_exist_ok=True)
        (root / "sources.lock.toml.fixture").rename(root / "sources.lock.toml")
        checkout = DependencyCheckout(root)
        checkout.write(
            "package-lock.json",
            json.dumps(
                {
                    "packages": {
                        "": {"devDependencies": {"tool": "3.0.0"}},
                        "node_modules/tool": {
                            "version": "3.0.0",
                            "resolved": "https://example.invalid/npm/tool-3.0.0.tgz",
                            "integrity": "sha512-"
                            + base64.b64encode(bytes.fromhex("11" * 64)).decode(),
                        },
                    },
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
        ):
            checkout.write(
                f"alpine/aports/{package}/APKBUILD",
                f'pkgname={package}\npkgver=1.0.1\npkgrel=0\narch="armv7"\n',
            )
        yield checkout


class DependencyInputTests:
    """Keep downloads bound to their original checksum and consumer cache path."""

    def test_environment_admits_existing_terminal_font_archive_without_target_selection(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Host rendering shares the bitmap package's native archive declaration."""
        dependency_checkout.write(
            "alpine/aports/fplinux-font-terminus/APKBUILD",
            'pkgname=fplinux-font-terminus\npkgver=4.49.1\narch="noarch"\n'
            'source="terminus-font-$pkgver.tar.gz::https://example.invalid/terminus-$pkgver.tar.gz"\n'
            f'sha512sums="{"3" * 128}  terminus-font-4.49.1.tar.gz"\n',
        )

        inputs = environment_inputs(dependency_checkout.root)

        key = "aport:fplinux-font-terminus:terminus-font-4.49.1.tar.gz"
        original = next(item for item in inputs if item.key == key)
        assert original == DependencyInput(
            key="aport:fplinux-font-terminus:terminus-font-4.49.1.tar.gz",
            url="https://example.invalid/terminus-4.49.1.tar.gz",
            sha256=None,
            size=None,
            destination="downloads/alpine/sources/terminus-font-4.49.1.tar.gz",
            purpose="aport-source",
            arch="noarch",
            checksum="3" * 128,
            algorithm="sha512",
        )
        selected_inputs, context = dependency_selection(dependency_checkout.root)
        assert sum(item.key == key for item in selected_inputs) == 1
        producers = context["producers"]
        assert isinstance(producers, list)
        assert "fplinux-font-terminus" not in producers

    def test_environment_admits_ui_font_originals_without_target_packages(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Offline setup and snapshots share the UI package's native checksum and cache slot."""
        dependency_checkout.write(
            "alpine/aports/fplinux-ui/APKBUILD",
            'pkgname=fplinux-ui\narch="armv7"\n'
            'source="face.ttf::https://example.invalid/face.ttf local.c"\n'
            f'sha512sums="{"1" * 128}  face.ttf\n{"2" * 128}  local.c"\n',
        )
        environment = environment_inputs(dependency_checkout.root)
        original = next(item for item in environment if item.key == "aport:fplinux-ui:face.ttf")
        assert original.url == "https://example.invalid/face.ttf"
        assert original.checksum == "1" * 128
        assert original.algorithm == "sha512"
        assert original.destination == "downloads/alpine/sources/face.ttf"
        assert original.purpose == "aport-source"
        inputs, context = dependency_selection(dependency_checkout.root)
        assert original in inputs
        assert sum(item.key == original.key for item in inputs) == 1
        producers = context["producers"]
        assert isinstance(producers, list)
        assert "fplinux-ui" not in producers
        target = dependency_checkout.root / "targets/test-phone/target.toml"
        declaration = tomlkit.parse(target.read_text(encoding="utf-8"))
        rootfs = declaration["rootfs"]
        assert isinstance(rootfs, Table)
        rootfs["packages"] = ["fplinux-ui"]
        target.write_text(tomlkit.dumps(declaration), encoding="utf-8")
        selected_inputs, selected_context = dependency_selection(dependency_checkout.root)
        selected_producers = selected_context["producers"]
        assert isinstance(selected_producers, list)
        assert "fplinux-ui" in selected_producers
        assert sum(item.key == original.key for item in selected_inputs) == 1
        assert original in selected_inputs

    def test_source_built_loaders_select_their_three_archives_without_ums_assets(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Offline preparation receives the three source archives consumed by the SC builder."""
        platform = dependency_checkout.root / "platforms/test-platform/platform.toml"
        platform.write_text(
            platform.read_text(encoding="utf-8")
            .replace('backend = "ums9117"', 'backend = "sc9820e"')
            .replace('vendor_source_lock = "support"\n', "")
            .replace('vendor_cache_name = "support.tar.gz"\n', ""),
            encoding="utf-8",
        )
        (dependency_checkout.root / "platforms/test-platform/loader/assets.lock.toml").unlink()
        source_fixture = _FIXTURES.parent / "sc9820e-manifest"
        target = dependency_checkout.root / "targets/test-phone/target.toml"
        target.write_text(
            (source_fixture / "target.toml")
            .read_text(encoding="utf-8")
            .replace('platform = "sc9820e"', 'platform = "test-platform"'),
            encoding="utf-8",
        )
        with (dependency_checkout.root / "sources.lock.toml").open("ab") as lock:
            lock.write(b"\n")
            lock.write((source_fixture / "sources.lock.toml").read_bytes())

        inputs, context = dependency_selection(dependency_checkout.root)

        assert not any(record.purpose in {"bootstrap-source", "loader-asset"} for record in inputs)
        actual = {record.key: record for record in inputs}
        assert actual["source:kernel:linux"].url == "https://example.invalid/linux.tar.xz"
        assert actual["source:host:host"].destination == "downloads/host.tar.gz"
        loader_inputs = {
            record.key: record for record in inputs if record.purpose == "loader-source"
        }
        assert set(loader_inputs) == {
            "source:chipram:loader",
            "source:u_boot:loader",
            "source:trusted_firmware:loader",
        }
        assert (
            loader_inputs["source:chipram:loader"].url == "https://example.invalid/chipram.tar.gz"
        )
        assert loader_inputs["source:u_boot:loader"].sha256 == "e" * 64
        assert loader_inputs["source:trusted_firmware:loader"].destination == (
            "downloads/loaders/trusted_firmware.tar.gz"
        )
        assets = context["assets"]
        assert isinstance(assets, dict)
        selected_assets = assets["test-phone"]
        assert isinstance(selected_assets, list)
        assert {record["role"] for record in selected_assets} == {
            "fdl1",
            "fdl2",
            "sml",
        }

    def test_quality_uploader_uses_the_existing_exact_source_projection(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Offline quality inputs retain the pinned uploader archive without a second lock."""
        records = {record.key: record for record in environment_inputs(dependency_checkout.root)}

        assert records["environment:spreadtrum-flash"] == DependencyInput(
            key="environment:spreadtrum-flash",
            url="https://example.invalid/spreadtrum-flash.tar.gz",
            sha256="a" * 64,
            size=None,
            destination="downloads/spreadtrum_flash.tar.gz",
            purpose="quality-source",
            arch="x86_64",
        )

    @pytest.mark.parametrize(
        ("field", "value", "attribute"),
        [
            ("archive_url", "https://example.invalid/repacked.tar.gz", "url"),
            ("archive_sha256", "b" * 64, "sha256"),
        ],
        ids=("archive-url", "archive-checksum"),
    )
    def test_quality_source_download_tracks_shared_archive_declarations(
        self,
        dependency_checkout: DependencyCheckout,
        field: str,
        value: str,
        attribute: str,
    ) -> None:
        """The shared source lock owns the archive consumed by environment preparation."""
        source = dependency_checkout.root / "sources.lock.toml"
        declarations = tomlkit.parse(source.read_text(encoding="utf-8"))
        quality_source = declarations["spreadtrum_flash"]
        assert isinstance(quality_source, Table)
        quality_source[field] = value
        source.write_text(declarations.as_string(), encoding="utf-8")

        records = {record.key: record for record in environment_inputs(dependency_checkout.root)}

        assert getattr(records["environment:spreadtrum-flash"], attribute) == value

    def test_changed_quality_source_projection_rejects_a_saved_dependency_selection(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Member checksum declarations stay strict even when the original archive is unchanged."""
        inputs, context = dependency_selection(dependency_checkout.root)
        saved = {"inputs": [{"input": asdict(item)} for item in inputs], "context": context}
        source = dependency_checkout.root / "sources.lock.toml"
        source.write_text(source.read_text().replace("b" * 64, "e" * 64))
        current_inputs, current_context = dependency_selection(dependency_checkout.root)

        assert current_inputs == inputs
        with pytest.raises(SystemExit, match="does not match this checkout"):
            require_checkout(saved, current_inputs, current_context)

    def test_bundled_npm_dependencies_keep_context_and_use_the_exact_parent_archive(self) -> None:
        """Direct and transitive bundled files are restored by their pinned package archive."""
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
        assert packages["node_modules/tool"]["bundleDependencies"] == ["parser"]
        assert packages["node_modules/tool/node_modules/parser"] == {
            "version": "2.0.0",
            "inBundle": True,
            "dependencies": {"helper": "3.0.0"},
        }
        assert packages["node_modules/tool/node_modules/parser/node_modules/helper"] == {
            "version": "3.0.0",
            "inBundle": True,
        }

    @pytest.mark.parametrize("bundled", [False, True], ids=("missing-normal-pin", "orphan-bundle"))
    def test_unpinned_npm_package_cannot_be_admitted_without_a_pinned_owner(
        self, tmp_path: Path, *, bundled: bool
    ) -> None:
        """A missing ordinary registry pin and an unowned bundle both remain errors."""
        lock = tmp_path / "package-lock.json"
        lock.write_text(
            json.dumps(
                {"packages": {"node_modules/other": {"version": "1", "inBundle": bundled}}}
            ),
            encoding="utf-8",
        )
        with pytest.raises(SystemExit, match=r"no exact .*registry input"):
            npm_declarations(
                lock, key_prefix="npm", cache_directory="downloads/npm", purpose="npm-package"
            )

    def test_downloads_retain_native_checksums_and_consumer_destinations(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """A SHA-512 source stays exact without claiming an unmeasured SHA-256."""
        dependency_checkout.write(
            "alpine/aports/fplinux-libtsm/APKBUILD",
            (_FIXTURES / "native-source.APKBUILD.fixture").read_text(),
        )
        inputs, _ = dependency_selection(dependency_checkout.root)
        actual = {record.key: record for record in inputs}
        aport = actual["aport:fplinux-libtsm:renamed.tar.gz"]
        assert (aport.url) == ("https://example.invalid/1.2/source-1.2.3.tar.gz")
        assert (aport.destination) == ("downloads/alpine/sources/renamed.tar.gz")
        assert (aport.checksum) == ("8" * 128)
        assert (aport.algorithm) == ("sha512")
        assert (aport.sha256) is None
        assert (actual["source:host:host"].destination) == ("downloads/host.tar.gz")
        assert (actual["source:kernel:linux"].destination) == ("downloads/linux/linux-6.0.tar.xz")
        assert (actual["alpine:minirootfs"].destination) == (
            "downloads/alpine/alpine-minirootfs-3.24.2-armv7.tar.gz"
        )
        assert (actual["asset:test-phone:fdl1"].destination) == ("downloads/loader.bin")

    def test_source_loop_expands_each_pinned_patch_in_order(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Patch declarations in a literal loop remain separate exact downloads."""
        dependency_checkout.write(
            "alpine/aports/fplinux-bash/APKBUILD",
            (_FIXTURES / "source-loop.APKBUILD.fixture").read_text(),
        )
        _, actual = dependency_selection(dependency_checkout.root)
        apports = actual["aports"]
        assert isinstance(apports, dict), "context must contain aport declarations"
        assert (apports["fplinux-bash"]["sources"]) == (
            [
                {
                    "file": "patch-001",
                    "url": "https://example.invalid/patch-001",
                    "sha512": "a" * 128,
                },
                {
                    "file": "patch-002",
                    "url": "https://example.invalid/patch-002",
                    "sha512": "b" * 128,
                },
            ]
        )

    def test_context_ignores_comments_key_order_and_unrelated_driver_bytes(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """An archive remains compatible with presentation and project source edits."""
        _, before = dependency_selection(dependency_checkout.root)
        path = dependency_checkout.root / "sources.lock.toml"
        path.write_text(
            path.read_text().replace(
                '[kernel]\nversion = "6.0"\nurl = "https://example.invalid/linux.tar.xz"',
                '# Comment\n[kernel]\nurl = "https://example.invalid/linux.tar.xz"\n'
                'version = "6.0"',
            )
        )
        dependency_checkout.write("platforms/test-platform/linux/driver.c", "int changed;\n")
        dependency_checkout.write(
            "alpine/aports/fplinux-unused/APKBUILD",
            'source="https://example.invalid/unused-unpinned.tar.gz"\n',
        )
        path = dependency_checkout.root / "alpine/aports/fplinux-base/APKBUILD"
        path.write_text("# Comment\n" + path.read_text())
        _, after = dependency_selection(dependency_checkout.root)
        assert (after) == (before)

    def test_platform_loader_pin_changes_download_selection_and_archive_context(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Changing the loader source pin invalidates the dependency archive selection."""
        declaration = (
            dependency_checkout.root / "platforms/test-platform/loader/assets.lock.toml"
        ).read_text()
        before_inputs, before_context = dependency_selection(dependency_checkout.root)
        dependency_checkout.write(
            "platforms/test-platform/loader/assets.lock.toml",
            declaration.replace("7" * 64, "a" * 64),
        )
        after_inputs, after_context = dependency_selection(dependency_checkout.root)

        before = {record.key: record for record in before_inputs}
        after = {record.key: record for record in after_inputs}
        assert before["asset:test-phone:fdl1"].sha256 == "7" * 64
        assert after["asset:test-phone:fdl1"].sha256 == "a" * 64
        assert after["asset:test-phone:fdl1"].destination == "downloads/loader.bin"
        assets = after_context["assets"]
        assert isinstance(assets, dict)
        assert assets["test-phone"][0]["output"] == [
            {
                "role": "fdl1",
                "path": "loader.bin",
                "sha256": "a" * 64,
            },
        ]
        assert before_context != after_context

    def test_context_tracks_selected_providers_and_source_sequences(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Provider choice and source application order belong to archive compatibility."""
        path = dependency_checkout.root / "alpine/aports/fplinux-base/APKBUILD"
        path.write_text(
            path.read_text() + 'provides="virtual=1"\nsource="first.patch second.patch"\n'
        )
        _, before = dependency_selection(dependency_checkout.root)
        path.write_text(path.read_text().replace("virtual=1", "virtual=2"))
        _, after = dependency_selection(dependency_checkout.root)
        assert (after) != (before)
        _, before = dependency_selection(dependency_checkout.root)
        path.write_text(
            path.read_text().replace("first.patch second.patch", "second.patch first.patch")
        )
        _, after = dependency_selection(dependency_checkout.root)
        assert (after) != (before)

    def test_environment_inventory_does_not_require_target_sources(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Environment setup can enumerate its exact closure independently of boards."""
        (dependency_checkout.root / "targets/test-phone/target.toml").unlink()
        records = {record.key: record for record in environment_inputs(dependency_checkout.root)}
        assert (records["environment:tool"].size) == (12)
        assert (records["environment:tool-source"].url) == (
            "https://example.invalid/tool-2.0.tar.gz"
        )
        assert (records["npm:node_modules/tool"].checksum) == ("11" * 64)
        assert (records["npm:node_modules/tool"].sha256) is None

    def test_locked_wheel_keeps_its_exact_bytes_and_cache_destination(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Setup and host Python consumers receive the same exact wheel declaration."""
        lock = dependency_checkout.root / "environment.lock.toml"
        with lock.open("a") as output:
            output.write((_FIXTURES / "pytest-wheel.toml.fixture").read_text())
        selected = [
            record
            for record in environment_inputs(dependency_checkout.root)
            if record.key == "environment:pytest"
        ]
        assert len(selected) == 1
        wheel = selected[0]
        assert wheel.url == "https://example.invalid/pytest-1.0-py3-none-any.whl"
        assert wheel.sha256 == "9" * 64
        assert wheel.size == 456
        assert wheel.destination == "downloads/environment/pytest/pytest-1.0-py3-none-any.whl"
        assert wheel.purpose == "environment-source"
        assert wheel.arch == "x86_64"

    def test_site_registry_inputs_are_separate_from_the_environment_closure(
        self, dependency_checkout: DependencyCheckout, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """Snapshots retain both locks while environment setup only selects its root packages."""
        dependency_checkout.write(
            "package.json", '{"engines":{"node":"24.21.0","npm":"12.2.0"}}\n'
        )
        dependency_checkout.write(
            ".github/workflows/pages.yml",
            (_FIXTURES / "pages.yml.fixture").read_text(),
        )
        dependency_checkout.write(
            "site/package-lock.json",
            json.dumps(
                {
                    "packages": {
                        "": {"dependencies": {"tool": "4.0.0"}},
                        "node_modules/tool": {
                            "version": "4.0.0",
                            "resolved": "https://example.invalid/site/tool-4.0.0.tgz",
                            "integrity": "sha512-"
                            + base64.b64encode(bytes.fromhex("22" * 64)).decode(),
                        },
                    }
                }
            ),
        )
        monkeypatch.setattr(common, "ROOT", dependency_checkout.root)
        snapshot_inputs, context = dependency_cli._checkout_declarations()  # noqa: SLF001
        snapshot = {item.key: item for item in snapshot_inputs}
        root_input = snapshot["npm:node_modules/tool"]
        site_input = snapshot["npm:site:node_modules/tool"]
        assert root_input.url == "https://example.invalid/npm/tool-3.0.0.tgz"
        assert root_input.checksum == "11" * 64
        assert root_input.purpose == "npm-package"
        assert root_input.destination.startswith("downloads/npm/")
        assert site_input.url == "https://example.invalid/site/tool-4.0.0.tgz"
        assert site_input.checksum == "22" * 64
        assert site_input.purpose == "site-npm-package"
        assert site_input.destination.startswith("downloads/site/npm/")
        assert context["site"]["consumer"]["node"] == "24.21.0"
        environment = environment_inputs(dependency_checkout.root)
        assert root_input in environment
        assert site_input not in environment

    def test_missing_or_dynamic_source_checksum_cannot_enter_inventory(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """Incomplete source declarations fail before an archive can claim completeness."""
        path = dependency_checkout.root / "alpine/aports/fplinux-libtsm/APKBUILD"
        dependency_checkout.write(
            path.relative_to(dependency_checkout.root).as_posix(),
            'pkgname=fplinux-libtsm\nsource="https://example.invalid/unpinned.tar.gz"\n',
        )
        with pytest.raises(SystemExit, match="external source has no checksum"):
            dependency_selection(dependency_checkout.root)
        dependency_checkout.write(
            path.relative_to(dependency_checkout.root).as_posix(),
            'pkgname=fplinux-libtsm\nsource="$(printf https://example.invalid/source)"\n',
        )
        with pytest.raises(SystemExit, match="unsupported command substitution"):
            dependency_selection(dependency_checkout.root)

    def test_changed_environment_input_changes_snapshot_selection(
        self, dependency_checkout: DependencyCheckout
    ) -> None:
        """A new locked download cannot reuse an archive containing different bytes."""
        before_inputs, before_context = dependency_selection(dependency_checkout.root)
        path = dependency_checkout.root / "environment.lock.toml"
        path.write_text(path.read_text().replace("e" * 64, "f" * 64))
        after_inputs, after_context = dependency_selection(dependency_checkout.root)
        assert before_inputs != after_inputs
        assert before_context != after_context
        assert (
            next(item for item in after_inputs if item.key == "environment:tool").sha256
            == "f" * 64
        )

    def test_records_reject_missing_digest_and_non_normalized_destinations(self) -> None:
        """Exact download records cannot describe unpinned bytes or escaping paths."""
        with pytest.raises(SystemExit, match="exact SHA-256 or SHA-512"):
            DependencyInput(
                "test", "https://example.invalid/file", None, None, "downloads/file", "test"
            )
        with pytest.raises(SystemExit, match="normalized relative path"):
            DependencyInput(
                "test", "https://example.invalid/file", "a" * 64, None, "downloads/../file", "test"
            )
