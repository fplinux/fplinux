# SPDX-License-Identifier: GPL-2.0-only
"""Measure small synthetic bundle artifacts using real archive and ELF bytes."""

from __future__ import annotations

import hashlib
import json
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.artifacts.bundles import CurrentBundle
from fplinux_cli.artifacts.footprint import FootprintError, compare_footprints, inspect_footprint

from tests.fixtures.artifact_footprint import FootprintFixture, kernel_with_initramfs, optional_apk

if TYPE_CHECKING:
    from pathlib import Path


class ArtifactFootprintTests:
    """Protect size attribution and comparisons at the host artifact reader boundary."""

    @staticmethod
    @pytest.mark.parametrize("compression", ["xz", "lz4"])
    def test_compressed_lower_reports_its_own_bytes_without_changing_attribution(
        tmp_path: Path, compression: str
    ) -> None:
        """Boot backing size and package ownership describe separate archive layers."""
        fixture = FootprintFixture(tmp_path)
        report = inspect_footprint(fixture.bundle(lower_compression=compression))
        lower = report["layers"]["ram_root"]
        assert (lower["filesystem"]) == ("squashfs")
        assert (lower["compression"]) == (compression)
        assert (lower["block_bytes"]) == (65536)
        assert (lower["bytes"]) == (120)
        assert (lower["sha256"]) == (hashlib.sha256(fixture.lower).hexdigest())
        assert (report["rootfs"]["source"]) == ("ram-squashfs-composition")
        assert (report["rootfs"]["packages"]["app-one"]["regular_payload_bytes"]) == (5)
        assert (report["layers"]["embedded_initramfs"]["cpio_bytes"]) == (len(fixture.initramfs))
        assert (len(fixture.initramfs)) != (len(fixture.composition))

    @staticmethod
    def test_shared_dependency_and_hardlinks_are_counted_once(tmp_path: Path) -> None:
        """Consumer chains explain shared bytes without charging them to either app."""
        fixture = FootprintFixture(tmp_path)
        report = inspect_footprint(fixture.bundle())
        root = report["rootfs"]
        packages = root["packages"]
        assert (packages["app-one"]["regular_payload_bytes"]) == (5)
        assert (packages["app-two"]["regular_payload_bytes"]) == (3)
        assert (packages["shared"]["regular_payload_bytes"]) == (7)
        assert (packages["shared"]["reason_paths"]) == (
            {
                "app-one": ["app-one", "shared"],
                "app-two": ["app-two", "shared"],
            }
        )
        assert (root["regular_payload_bytes"]) == (len(fixture.database) + 16 + 15)
        assert (root["symlink_payload_bytes"]) == (3)
        assert (root["file_page_model"]["rounded_regular_bytes"]) == (20480)
        assert (root["files"]["/bin/one-link"]["size"]) == (5)
        assert (root["files"]["/bin/one-link"]["accounted_bytes"]) == (0)
        assert (root["unowned_payload_bytes"]) == (len(fixture.database) + 16 + 3)
        assert (report["optional_apks"]["apks/optional.apk"]["base_dependency_edges"]) == (
            [
                {"requirement": "so:libshared.so.1", "providers": ["shared"]},
            ]
        )
        assert ("optional") not in (packages)
        assert (
            report["optional_apks"]["apks/optional.apk"]["preinstalled_dependency_packages"]
        ) == (["shared"])

    @staticmethod
    def test_install_if_explains_automatically_selected_service_package(tmp_path: Path) -> None:
        """Both installed triggers explain an automatic subpackage's presence."""
        fixture = FootprintFixture(tmp_path)
        fixture.database += b"P:service\nV:1-r0\ni:app-one app-two\nF:etc/init.d\nR:service\n\n"
        report = inspect_footprint(
            fixture.bundle(
                extra=[("etc/init.d/service", b"service", 0o100755, 9, 1)],
            )
        )
        package = report["rootfs"]["packages"]["service"]
        assert (package["selected_by"]) == ([])
        assert (package["reason_paths"]) == (
            {
                "app-one": ["app-one", "service"],
                "app-two": ["app-two", "service"],
            }
        )
        assert (package["regular_payload_bytes"]) == (7)

    @staticmethod
    def test_optional_dependency_chain_keeps_preinstalled_library_in_base(tmp_path: Path) -> None:
        """An external add-on inherits an external program's existing base dependencies."""
        fixture = FootprintFixture(tmp_path)
        report = inspect_footprint(
            fixture.bundle(
                extra_apks={
                    "apks/addon.apk": optional_apk(
                        b"pkgname = addon\npkgver = 1-r0\ndepend = optional\n",
                    ),
                }
            )
        )
        addon = report["optional_apks"]["apks/addon.apk"]
        assert (addon["preinstalled_dependency_packages"]) == (["shared"])
        assert (addon["optional_dependency_edges"]) == (
            [
                {"requirement": "optional", "providers": ["optional"]},
            ]
        )
        assert (report["rootfs"]["packages"]["shared"]["regular_payload_bytes"]) == (7)
        assert ("addon") not in (report["rootfs"]["packages"])

    @staticmethod
    def test_compression_change_preserves_content_identity(tmp_path: Path) -> None:
        """Actual gzip and XZ streams describe the same files and ownership."""
        fixture = FootprintFixture(tmp_path)
        before = inspect_footprint(fixture.bundle(compression="gzip"))
        after = inspect_footprint(fixture.bundle(compression="xz"))
        delta = compare_footprints(before, after)
        assert delta["same_rootfs_content"]
        assert delta["initramfs_compression_changed"]
        assert (delta["files"]) == ({"added": {}, "removed": {}, "changed": {}})
        assert (delta["rootfs_byte_delta"]["regular_payload_bytes"]) == (0)
        assert (before["layers"]["embedded_initramfs"]["compression"]) == ("gzip")
        assert (after["layers"]["embedded_initramfs"]["compression"]) == ("xz")

    @staticmethod
    def test_package_and_file_changes_have_forward_and_reverse_deltas(tmp_path: Path) -> None:
        """Version changes and additions have explicit forward and reverse deltas."""
        fixture = FootprintFixture(tmp_path)
        before = inspect_footprint(fixture.bundle())
        fixture.database = fixture.database.replace(b"P:app-two\nV:1-r0", b"P:app-two\nV:2-r0")
        fixture.database += b"P:added\nV:1-r0\nF:bin\nR:new\n\n"
        after = inspect_footprint(fixture.bundle(extra=[("bin/new", b"new!", 0o100755, 9, 1)]))
        delta = compare_footprints(before, after)
        assert not (delta["same_rootfs_content"])
        assert (set(delta["packages"]["added"])) == ({"added"})
        assert (delta["packages"]["changed"]["app-two"]["after"]["version"]) == ("2-r0")
        assert (set(delta["files"]["added"])) == ({"/bin/new"})
        assert ("/lib/apk/db/installed") in (delta["files"]["changed"])
        reverse = compare_footprints(after, before)
        assert (set(reverse["packages"]["removed"])) == ({"added"})
        assert (set(reverse["files"]["removed"])) == ({"/bin/new"})

    @staticmethod
    def test_external_root_uses_composition_without_claiming_embedded_initramfs(
        tmp_path: Path,
    ) -> None:
        """A storage image and its composition are separate from the boot kernel."""
        report = inspect_footprint(FootprintFixture(tmp_path).bundle(external=True))
        assert (report["layers"]["embedded_initramfs"]) is None
        assert (report["rootfs"]["source"]) == ("external-root-composition")
        assert ("card.img.gz") in (report["layers"]["boot_artifact_bytes"])
        assert ("debug/rootfs.cpio") in (report["layers"]["host_debug_file_bytes"])

    @staticmethod
    def test_changed_artifact_is_rejected_before_reporting_its_size(tmp_path: Path) -> None:
        """A stale or incomplete selected bundle cannot yield misleading measurements."""
        fixture = FootprintFixture(tmp_path)
        bundle = fixture.bundle()
        (tmp_path / "debug/zImage").write_bytes(b"wrong-kernel")
        with pytest.raises(FootprintError, match="missing or changed: debug/zImage"):
            inspect_footprint(bundle)

    @staticmethod
    def test_embedded_payload_must_match_published_boot_archive(tmp_path: Path) -> None:
        """Valid file hashes alone do not establish which boot archive the ELF embeds."""
        fixture = FootprintFixture(tmp_path)
        bundle = fixture.bundle()
        elf = kernel_with_initramfs(fixture.composition, "gzip")
        (tmp_path / "debug/vmlinux").write_bytes(elf)
        manifest: dict[str, Any] = json.loads(bundle.manifest_bytes)
        manifest["files"]["debug/vmlinux"] = {
            "size": len(elf),
            "sha256": hashlib.sha256(elf).hexdigest(),
        }
        replaced = CurrentBundle(
            bundle.path,
            bundle.generation,
            "",
            json.dumps(manifest).encode(),
        )
        with pytest.raises(FootprintError, match="differs from the published root"):
            inspect_footprint(replaced)
