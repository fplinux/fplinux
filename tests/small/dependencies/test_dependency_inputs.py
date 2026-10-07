# SPDX-License-Identifier: GPL-2.0-only
"""Host tests for exact dependency declarations and semantic archive context."""

from __future__ import annotations

import base64
import json
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from fplinux_cli import common
from fplinux_cli.cli import dependencies as dependency_cli
from fplinux_cli.dependencies.inputs import (
    DependencyInput,
    dependency_selection,
    environment_inputs,
)

if TYPE_CHECKING:
    from collections.abc import Iterator


class DependencyInputTests:
    """Keep downloads bound to their original checksum and consumer cache path."""

    @pytest.fixture(autouse=True)
    def prepare_checkout(self) -> Iterator[None]:
        """Prepare a synthetic checkout with literal external declarations."""
        temporary = tempfile.TemporaryDirectory()
        with temporary:
            self.root = Path(temporary.name)
            self._write(
                "container.lock.toml",
                '[kern]\nversion = "1.0.0"\n'
                'archive_url = "https://example.invalid/kern.tar.gz"\n'
                f'archive_sha256 = "{"a" * 64}"\n'
                f'binary_sha256 = "{"b" * 64}"\n'
                '[oci]\nbase_rootfs_url = "https://example.invalid/base.tar.gz"\n'
                f'base_rootfs_sha256 = "{"c" * 64}"\n',
            )
            self._write(
                "Containerfile",
                "ARG TOOL_VERSION=2.0\n"
                f"ARG TOOL_SHA256={'d' * 64}\n"
                "RUN apk add --no-cache tool=2.0-r0\n"
                'RUN curl --output "${archive}" '
                '"https://example.invalid/tool-${TOOL_VERSION}.tar.gz"; '
                'printf "%s %s" "${TOOL_SHA256}" "${archive}" | sha256sum -c -\n',
            )
            self._write(
                "environment.lock.toml",
                '[selection]\narch = "x86_64"\napk_groups = [["tool=2.0-r0"]]\n'
                '[[input]]\nkey = "environment:tool"\n'
                'url = "https://example.invalid/tool-2.0-r0.apk"\n'
                f'sha256 = "{"e" * 64}"\nbytes = 12\n'
                'destination = "downloads/environment/apk/main/x86_64/tool-2.0-r0.apk"\n'
                'purpose = "container-package"\narch = "x86_64"\n'
                '[input.package]\nname = "tool"\nversion = "2.0-r0"\n'
                'provides = ["cmd:tool=2.0-r0"]\ndepends = []\n'
                '[[key]]\nfile = "test.rsa.pub"\n'
                f'sha256 = "{"f" * 64}"\nbytes = 10\n'
                'source = "container:base-rootfs"\nmember = "etc/apk/keys/test.rsa.pub"\n',
            )
            self._write(
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
            self._write(
                "alpine.lock.toml",
                'release = "3.24.2"\nbranch = "v3.24"\narch = "armv7"\n'
                'triplet = "armv7-alpine-linux-musleabihf"\n'
                '[repositories]\nmain = "https://example.invalid/main/armv7"\n'
                'community = "https://example.invalid/community/armv7"\n'
                '[minirootfs]\nurl = "https://example.invalid/target.tar.gz"\n'
                f'sha256 = "{"2" * 64}"\nbytes = 2\n'
                '[runtime]\npackages = ["runtime-1-r0.apk"]\n[runtime.additions]\n'
                '[sysroot]\npackages = ["runtime-1-r0.apk"]\n'
                '[[package]]\nrepository = "main"\nfile = "runtime-1-r0.apk"\n'
                f'sha256 = "{"3" * 64}"\nbytes = 3\n',
            )
            self._write(
                "sources.lock.toml",
                '[kernel]\nversion = "6.0"\nurl = "https://example.invalid/linux.tar.xz"\n'
                f'sha256 = "{"4" * 64}"\n'
                '[support]\narchive_url = "https://example.invalid/support.tar.gz"\n'
                f'archive_sha256 = "{"5" * 64}"\n'
                '[host]\narchive_url = "https://example.invalid/host.tar.gz"\n'
                f'archive_sha256 = "{"6" * 64}"\n',
            )
            self._write(
                "platforms/test-platform/platform.toml",
                "[rootfs]\npackages = []\n[bundle]\npackages = []\n"
                '[linux]\nsource_lock = "kernel"\n'
                '[bootstrap]\nvendor_source_lock = "support"\n'
                'vendor_cache_name = "support.tar.gz"\n'
                '[[host.tools]]\ntype = "make-archive"\nname = "loader"\n'
                'source_lock = "host"\ncache_name = "host.tar.gz"\n'
                'archive_prefix = "host/"\nmembers = []\n',
            )
            self._write(
                "targets/test-phone/target.toml",
                'platform = "test-platform"\n[rootfs]\npackages = []\n[bundle]\npackages = []\n',
            )
            self._write(
                "profiles/default/profile.toml",
                '[rootfs]\npackages = []\n[bootstrap]\nkind = "linux"\n[uboot]\nkind = "none"\n',
            )
            self._write(
                "targets/test-phone/loader/assets.lock.toml",
                '[[source]]\nid = "loader"\nkind = "file"\n'
                'url = "https://example.invalid/loader.bin"\n'
                f'sha256 = "{"7" * 64}"\ncache_name = "loader.bin"\nlicense = "MIT"\n'
                '[[source.output]]\nrole = "fdl1"\npath = "loader.bin"\n'
                f'sha256 = "{"7" * 64}"\n',
            )
            for package in (
                "fplinux-base",
                "fplinux-bash",
                "fplinux-busybox",
                "fplinux-ncurses",
                "fplinux-openrc",
                "fplinux-readline",
                "fplinux-terminal",
                "fplinux-input",
                "fplinux-libdrm",
                "fplinux-libtsm",
                "fplinux-libudev",
                "fplinux-libxkbcommon",
            ):
                self._write(
                    f"alpine/aports/{package}/APKBUILD",
                    f'pkgname={package}\npkgver=1.0.1\npkgrel=0\narch="armv7"\n',
                )
            yield

    def _write(self, name: str, text: str) -> None:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_downloads_retain_native_checksums_and_consumer_destinations(self) -> None:
        """A SHA-512 source stays exact without claiming an unmeasured SHA-256."""
        self._write(
            "alpine/aports/fplinux-libtsm/APKBUILD",
            "pkgname=fplinux-libtsm\npkgver=1.2.3\narch=armv7\n"
            "_series=${pkgver%.*}\n"
            'source="renamed.tar.gz::https://example.invalid/$_series/source-$pkgver.tar.gz"\n'
            f'sha512sums="{"8" * 128}  renamed.tar.gz"\n',
        )
        inputs, _ = dependency_selection(self.root)
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
        assert (actual["asset:test-phone:loader"].destination) == ("downloads/loader.bin")

    def test_source_loop_expands_each_pinned_patch_in_order(self) -> None:
        """Patch declarations in a literal loop remain separate exact downloads."""
        self._write(
            "alpine/aports/fplinux-bash/APKBUILD",
            'pkgname=fplinux-bash\npkgver=1.0\narch=armv7\nsource=""\n'
            "for _patch in 001 002; do\n"
            '  source="$source https://example.invalid/patch-$_patch"\ndone\n'
            f'sha512sums="{"a" * 128} patch-001\n{"b" * 128} patch-002"\n',
        )
        _, actual = dependency_selection(self.root)
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

    def test_context_ignores_comments_key_order_and_unrelated_driver_bytes(self) -> None:
        """An archive remains compatible with presentation and project source edits."""
        _, before = dependency_selection(self.root)
        path = self.root / "sources.lock.toml"
        path.write_text(
            path.read_text().replace(
                '[kernel]\nversion = "6.0"\nurl = "https://example.invalid/linux.tar.xz"',
                '# Comment\n[kernel]\nurl = "https://example.invalid/linux.tar.xz"\n'
                'version = "6.0"',
            )
        )
        self._write("platforms/test-platform/linux/driver.c", "int changed;\n")
        self._write(
            "alpine/aports/fplinux-unused/APKBUILD",
            'source="https://example.invalid/unused-unpinned.tar.gz"\n',
        )
        path = self.root / "alpine/aports/fplinux-base/APKBUILD"
        path.write_text("# Comment\n" + path.read_text())
        _, after = dependency_selection(self.root)
        assert (after) == (before)

    def test_context_tracks_selected_providers_and_source_sequences(self) -> None:
        """Provider choice and source application order belong to archive compatibility."""
        path = self.root / "alpine/aports/fplinux-base/APKBUILD"
        path.write_text(
            path.read_text() + 'provides="virtual=1"\nsource="first.patch second.patch"\n'
        )
        _, before = dependency_selection(self.root)
        path.write_text(path.read_text().replace("virtual=1", "virtual=2"))
        _, after = dependency_selection(self.root)
        assert (after) != (before)
        _, before = dependency_selection(self.root)
        path.write_text(
            path.read_text().replace("first.patch second.patch", "second.patch first.patch")
        )
        _, after = dependency_selection(self.root)
        assert (after) != (before)

    def test_environment_inventory_does_not_require_target_sources(self) -> None:
        """Environment setup can enumerate its exact closure independently of boards."""
        (self.root / "targets/test-phone/target.toml").unlink()
        records = {record.key: record for record in environment_inputs(self.root)}
        assert (records["environment:tool"].size) == (12)
        assert (records["container:tool"].url) == ("https://example.invalid/tool-2.0.tar.gz")
        assert (records["npm:node_modules/tool"].checksum) == ("11" * 64)
        assert (records["npm:node_modules/tool"].sha256) is None

    def test_site_registry_inputs_are_separate_from_the_environment_closure(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """Snapshots retain both locks while environment setup only selects its root packages."""
        self._write("package.json", '{"engines":{"node":"24.21.0","npm":"12.2.0"}}\n')
        self._write(
            ".github/workflows/pages.yml",
            "jobs:\n  build:\n    runs-on: ubuntu-24.04\n    steps:\n"
            '      - with:\n          node-version: "24.21.0"\n'
            "      - run: npm install --global npm@12.2.0\n",
        )
        self._write(
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
        monkeypatch.setattr(common, "ROOT", self.root)
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
        environment = environment_inputs(self.root)
        assert root_input in environment
        assert site_input not in environment

    def test_missing_or_dynamic_source_checksum_cannot_enter_inventory(self) -> None:
        """Incomplete source declarations fail before an archive can claim completeness."""
        path = self.root / "alpine/aports/fplinux-libtsm/APKBUILD"
        self._write(
            path.relative_to(self.root).as_posix(),
            'pkgname=fplinux-libtsm\nsource="https://example.invalid/unpinned.tar.gz"\n',
        )
        with pytest.raises(SystemExit, match="external source has no checksum"):
            dependency_selection(self.root)
        self._write(
            path.relative_to(self.root).as_posix(),
            'pkgname=fplinux-libtsm\nsource="$(printf https://example.invalid/source)"\n',
        )
        with pytest.raises(SystemExit, match="unsupported command substitution"):
            dependency_selection(self.root)

    def test_changed_environment_selection_rejects_stale_closure(self) -> None:
        """A changed direct package pin cannot reuse a differently selected closure."""
        path = self.root / "Containerfile"
        path.write_text(path.read_text().replace("tool=2.0-r0", "tool=3.0-r0"))
        with pytest.raises(SystemExit, match="selection differs from Containerfile"):
            environment_inputs(self.root)

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
