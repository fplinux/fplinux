# SPDX-License-Identifier: GPL-2.0-only
"""Alpine registration and receipt invalidation scenarios."""

from __future__ import annotations

import io
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli.alpine import aports as alpine_aports
from fplinux_cli.alpine import packages as alpine_packages
from fplinux_cli.alpine import recipes as alpine_recipes
from fplinux_cli.alpine import selection as alpine_selection
from fplinux_cli.workspace import (
    build_inputs as workspace_build_inputs,
)
from fplinux_cli.workspace import (
    capture as workspace_capture,
)
from fplinux_cli.workspace import (
    staging as workspace_staging,
)

from tests import ROOT

if TYPE_CHECKING:
    from collections.abc import Iterator


class RegistrationContext:
    """Own copied host modules and independent package cache declarations."""

    def __init__(self, directory: Path) -> None:
        """Create a source checkout inside the scenario's temporary directory."""
        self.directory = directory
        self.root = directory / "source"
        self.repository = ROOT
        shutil.copytree(
            self.repository / "scripts/fplinux_cli",
            self.root / "scripts/fplinux_cli",
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        self.names: tuple[str, ...] = (
            "fplinux-package-a",
            "fplinux-package-b",
            "fplinux-library-c",
            "fplinux-library-d",
        )
        self.selected: tuple[str, ...] = self.names[:2]
        for name in self.names:
            self.write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
        self.write("alpine.lock.toml", b'[minirootfs]\nsource = "alpine_target"\n')
        self.write(
            "sources.lock.toml",
            (ROOT / "tests/fixtures/alpine/sources.lock.toml").read_bytes(),
        )
        self.write("alpine/abuild.conf", b"fixture abuild\n")
        self.write("alpine/ramroot-init.sh", b"#!/bin/sh\nexit 0\n")
        self.registration = self.root / "scripts/fplinux_cli/alpine/registration.py"

    def write(self, relative: str, contents: bytes) -> Path:
        """Write a controlled declaration or source inside the owned copied checkout."""
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def state(
        self,
        *,
        root: Path | None = None,
        names: tuple[str, ...] | None = None,
        selected: tuple[str, ...] | None = None,
    ) -> dict[str, Any]:
        """Run actual recipes and the receipt consumer without any build processes."""
        result = subprocess.run(
            [
                sys.executable,
                "-I",
                str(
                    ROOT / "tests/small/alpine/fixtures/registration/alpine_registration_state.py"
                ),
                str(self.root if root is None else root),
                json.dumps(self.names if names is None else names),
                json.dumps(self.selected if selected is None else selected),
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        assert (result.returncode) == (0), result.stderr
        state = json.loads(result.stdout)
        assert isinstance(state, dict)
        return dict(state)

    def seed_receipts(self) -> dict[str, Any]:
        """Publish ordinary output receipts for fixture APKs containing real package metadata."""
        state = self.state()
        for name, record in state["packages"].items():
            slot = self.root / ".cache/apks" / name
            slot.mkdir(parents=True, exist_ok=True)
            apk = slot / f"{name}-1-r0.apk"
            metadata = f"pkgname = {name}\n".encode()
            info = tarfile.TarInfo(".PKGINFO")
            info.size = len(metadata)
            with tarfile.open(apk, "w:gz") as archive:
                archive.addfile(info, io.BytesIO(metadata))
            alpine_packages._write_package_receipt(slot, record["recipe"], [apk])  # noqa: SLF001
        seeded = self.state()
        for name in self.names:
            assert seeded["packages"][name]["cache_hit"], name
        return seeded

    def declare_registration(self, declarations: str) -> None:
        """Append fixture-owned assignments to the copied registration module."""
        source = self.registration.read_text(encoding="utf-8")
        self.registration.write_text(source + "\n" + declarations + "\n", encoding="utf-8")


@pytest.fixture
def registered_sources(tmp_path: Path) -> Iterator[RegistrationContext]:
    """Release the copied checkout and generated package cache after each scenario."""
    with tempfile.TemporaryDirectory(dir=tmp_path) as temporary:
        yield RegistrationContext(Path(temporary))


class AlpineRegistrationTests:
    """Registration edits preserve unrelated APK receipts in isolated source trees."""

    @pytest.mark.parametrize(
        ("source", "affected", "staged_packages"),
        [
            pytest.param(
                "alpine/aports/fplinux-libweston/presentation-completion.patch",
                ("fplinux-libweston", "fplinux-libweston-gl"),
                ("fplinux-libweston", "fplinux-libweston-gl"),
                id="weston-abi-patch",
            ),
            pytest.param(
                "include/fplinux/fplinux-weston-presentation.h",
                ("fplinux-libweston", "fplinux-libweston-gl"),
                ("fplinux-libweston", "fplinux-libweston-gl"),
                id="weston-abi-header",
            ),
            pytest.param(
                "alpine/aports/fplinux-mesa/neon-clang.patch",
                (
                    "fplinux-mesa",
                    "fplinux-mesa-gbm",
                    "fplinux-libweston",
                    "fplinux-libweston-gl",
                ),
                (
                    "fplinux-mesa",
                    "fplinux-mesa-gbm",
                    "fplinux-mesa-headers",
                    "fplinux-mesa-gbm-dev",
                ),
                id="mesa-neon-patch",
            ),
        ],
    )
    def test_shared_graphics_edits_revoke_consumer_receipts_and_reach_aport_stages(
        self,
        registered_sources: RegistrationContext,
        source: str,
        affected: tuple[str, ...],
        staged_packages: tuple[str, ...],
    ) -> None:
        """Shared graphics bytes reach both producers and invalidate only causal APKs."""
        registered_sources.names = (
            "fplinux-libweston",
            "fplinux-libweston-gl",
            "fplinux-mesa",
            "fplinux-mesa-gbm",
            "fplinux-present",
        )
        registered_sources.selected = registered_sources.names[:4]
        for name in alpine_selection.aport_build_order(registered_sources.names):
            registered_sources.write(
                f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode()
            )
            for shared in alpine_selection.shared_aport_sources(name, registered_sources.root):
                registered_sources.write(
                    shared.relative_to(registered_sources.root).as_posix(), b"shared input\n"
                )
        registered_sources.write(source, b"original graphics input\n")
        before = registered_sources.seed_receipts()

        registered_sources.write("README.md", b"updated documentation\n")
        assert registered_sources.state() == before

        changed_bytes = b"changed graphics input\n"
        registered_sources.write(source, changed_bytes)
        after = registered_sources.state()

        for name in registered_sources.names:
            if name in affected:
                assert before["packages"][name]["recipe"] != after["packages"][name]["recipe"]
                assert not after["packages"][name]["cache_hit"], name
            else:
                assert before["packages"][name] == after["packages"][name], name
                assert after["packages"][name]["cache_hit"], name
        for name in staged_packages:
            stage = registered_sources.directory / "stages" / name
            alpine_aports.materialize_aport_sources(name, registered_sources.root, stage)
            assert (stage / Path(source).name).read_bytes() == changed_bytes, name
            if name in {"fplinux-mesa-headers", "fplinux-mesa-gbm-dev"}:
                assert (stage / "APKBUILD").read_bytes() == b"pkgname=fplinux-mesa-gbm\n"
        assert before["rootfs"] != after["rootfs"]

    @pytest.mark.parametrize(
        ("source", "changed_bytes"),
        [
            pytest.param(
                "lib/fplinux/fplinux-ui-profile.c",
                b"unsigned int palette_background = 0x30343b;\n",
                id="palette-source",
            ),
            pytest.param(
                "include/fplinux/fplinux-ui-profile.h",
                b"#define PROFILE_BACKGROUND 0x30343b\n",
                id="palette-header",
            ),
        ],
    )
    def test_palette_edits_revoke_consumer_receipts_and_reach_aport_stages(
        self, registered_sources: RegistrationContext, source: str, changed_bytes: bytes
    ) -> None:
        """Palette consumers stage new bytes and reject old APKs while unrelated APKs hit."""
        consumers = (
            "fplinux-terminal",
            "fplinux-brightness-ui",
            "fplinux-session",
            "fplinux-ui",
        )
        unrelated = "fplinux-present"
        registered_sources.names = (*consumers, unrelated)
        registered_sources.selected = consumers
        for name in alpine_selection.aport_build_order(registered_sources.names):
            registered_sources.write(
                f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode()
            )
            for shared in alpine_selection.shared_aport_sources(name, registered_sources.root):
                registered_sources.write(
                    shared.relative_to(registered_sources.root).as_posix(), b"shared input\n"
                )
        registered_sources.write(
            "lib/fplinux/fplinux-ui-profile.c",
            b"unsigned int palette_background = 0x20242b;\n",
        )
        registered_sources.write(
            "include/fplinux/fplinux-ui-profile.h", b"#define PROFILE_BACKGROUND 0x20242b\n"
        )
        before = registered_sources.seed_receipts()

        registered_sources.write("README.md", b"updated documentation\n")
        assert registered_sources.state() == before

        registered_sources.write(source, changed_bytes)
        after = registered_sources.state()

        for name in consumers:
            assert before["packages"][name]["recipe"] != after["packages"][name]["recipe"], name
            assert not after["packages"][name]["cache_hit"], name
            stage = registered_sources.directory / "stages" / name
            alpine_aports.materialize_aport_sources(name, registered_sources.root, stage)
            assert (stage / Path(source).name).read_bytes() == changed_bytes, name
        assert before["packages"][unrelated] == after["packages"][unrelated]
        assert after["packages"][unrelated]["cache_hit"]
        assert before["rootfs"] != after["rootfs"]

    def test_unrelated_registration_preserves_apk_recipe_and_receipt_hit(
        self, registered_sources: RegistrationContext
    ) -> None:
        """A newly declared dependency rebuilds its consumer while another APK stays reusable."""
        before = registered_sources.seed_receipts()

        registered_sources.declare_registration(
            'LOCAL_BUILD_DEPENDENCIES["fplinux-package-a"] = ("fplinux-library-c",)'
        )
        after = registered_sources.state()

        assert (
            (before["packages"][registered_sources.names[0]]["recipe"])
            != (after["packages"][registered_sources.names[0]]["recipe"])
        )
        assert not (after["packages"][registered_sources.names[0]]["cache_hit"])
        for name in registered_sources.names[1:]:
            assert (before["packages"][name]["recipe"]) == (after["packages"][name]["recipe"])
            assert after["packages"][name]["cache_hit"], name

    def test_dependency_edges_invalidate_even_when_transitive_library_set_is_unchanged(
        self,
        registered_sources: RegistrationContext,
    ) -> None:
        """Moving a dependency edge changes the consuming APKs despite identical library bytes."""
        registered_sources.declare_registration(
            'LOCAL_BUILD_DEPENDENCIES["fplinux-package-a"] = '
            '("fplinux-library-c", "fplinux-library-d")'
        )
        before = registered_sources.seed_receipts()

        registered_sources.declare_registration(
            'LOCAL_BUILD_DEPENDENCIES["fplinux-package-a"] = ("fplinux-library-c",)\n'
            'LOCAL_BUILD_DEPENDENCIES["fplinux-library-c"] = ("fplinux-library-d",)'
        )
        after = registered_sources.state()

        for name in (registered_sources.names[0], registered_sources.names[2]):
            assert (before["packages"][name]["recipe"]) != (after["packages"][name]["recipe"])
            assert not (after["packages"][name]["cache_hit"]), name
        for name in (registered_sources.names[1], registered_sources.names[3]):
            assert (before["packages"][name]["recipe"]) == (after["packages"][name]["recipe"])
            assert after["packages"][name]["cache_hit"], name
        assert (before["rootfs"]) != (after["rootfs"])

    @pytest.mark.parametrize(
        "source",
        [
            pytest.param("alpine/aports/fplinux-library-d/APKBUILD", id="library-aport"),
            pytest.param("lib/fplinux/library-d.c", id="shared-library-source"),
        ],
    )
    def test_transitive_dependency_and_shared_source_edits_revoke_consumer_receipts(
        self, registered_sources: RegistrationContext, source: str
    ) -> None:
        """Library edits revoke transitive consumers while an unrelated APK remains reusable."""
        registered_sources.declare_registration(
            'LOCAL_BUILD_DEPENDENCIES["fplinux-package-a"] = ("fplinux-library-c",)\n'
            'LOCAL_BUILD_DEPENDENCIES["fplinux-library-c"] = ("fplinux-library-d",)'
        )
        registered_sources.write("lib/fplinux/library-d.c", b"library shared input\n")
        registered_sources.declare_registration(
            'SHARED_APORT_SOURCES["fplinux-library-d"] = ("lib/fplinux/library-d.c",)'
        )
        changed_source = registered_sources.root / source

        before = registered_sources.seed_receipts()
        changed_source.write_bytes(changed_source.read_bytes() + b"changed input\n")
        after = registered_sources.state()
        for name in (
            registered_sources.names[0],
            registered_sources.names[2],
            registered_sources.names[3],
        ):
            assert (before["packages"][name]["recipe"]) != (after["packages"][name]["recipe"])
            assert not (after["packages"][name]["cache_hit"]), name
        unrelated = registered_sources.names[1]
        assert (
            (before["packages"][unrelated]["recipe"]) == (after["packages"][unrelated]["recipe"])
        )
        assert after["packages"][unrelated]["cache_hit"]
        assert (before["rootfs"]) != (after["rootfs"])

    def test_equivalent_producer_aliases_and_dependency_order_preserve_receipts(
        self, registered_sources: RegistrationContext
    ) -> None:
        """Equivalent declarations of the same producer graph do not rebuild its outputs."""
        registered_sources.declare_registration(
            'SUBPACKAGE_APORTS["fplinux-library-c-extra"] = "fplinux-library-c"'
        )
        registered_sources.declare_registration(
            'LOCAL_BUILD_DEPENDENCIES["fplinux-package-a"] = '
            '("fplinux-library-c", "fplinux-library-c-extra", "fplinux-library-d")'
        )
        before = registered_sources.seed_receipts()

        registered_sources.declare_registration(
            'LOCAL_BUILD_DEPENDENCIES["fplinux-package-a"] = '
            '("fplinux-library-d", "fplinux-library-c")'
        )
        assert (before) == (registered_sources.state())

    def test_shared_source_owner_changes_revoke_affected_receipts(
        self, registered_sources: RegistrationContext
    ) -> None:
        """Moving a shared input between producers changes recipes despite an unchanged union."""
        registered_sources.declare_registration(
            'LOCAL_BUILD_DEPENDENCIES["fplinux-package-a"] = ("fplinux-library-c",)'
        )
        registered_sources.write("lib/fplinux/shared.c", b"same shared source\n")
        registered_sources.declare_registration(
            'SHARED_APORT_SOURCES["fplinux-package-a"] = ("lib/fplinux/shared.c",)'
        )
        before = registered_sources.seed_receipts()

        registered_sources.declare_registration(
            'del SHARED_APORT_SOURCES["fplinux-package-a"]\n'
            'SHARED_APORT_SOURCES["fplinux-library-c"] = ("lib/fplinux/shared.c",)'
        )
        after = registered_sources.state()

        for name in (registered_sources.names[0], registered_sources.names[2]):
            assert (before["packages"][name]["recipe"]) != (after["packages"][name]["recipe"])
            assert not (after["packages"][name]["cache_hit"]), name
        for name in (registered_sources.names[1], registered_sources.names[3]):
            assert (before["packages"][name]["recipe"]) == (after["packages"][name]["recipe"])
            assert after["packages"][name]["cache_hit"], name
        assert (before["rootfs"]) != (after["rootfs"])

    def test_recipe_implementation_edit_revokes_all_apk_receipts(
        self, registered_sources: RegistrationContext
    ) -> None:
        """Algorithm-module changes remain a global input for every APK."""
        before = registered_sources.seed_receipts()
        implementation = registered_sources.root / "scripts/fplinux_cli/alpine/recipes.py"
        implementation.write_text(
            implementation.read_text(encoding="utf-8") + "\n# Fixture algorithm-module edit.\n",
            encoding="utf-8",
        )
        after = registered_sources.state()

        for name in registered_sources.names:
            assert (before["packages"][name]["recipe"]) != (after["packages"][name]["recipe"])
            assert not (after["packages"][name]["cache_hit"]), name
        assert (before["rootfs"]) != (after["rootfs"])

    def test_rootfs_selection_changes_composition_and_preserves_apk_receipts(
        self, registered_sources: RegistrationContext
    ) -> None:
        """Changing only the selected composition reuses every unchanged APK."""
        before = registered_sources.seed_receipts()
        after = registered_sources.state(selected=registered_sources.selected[:1])
        assert (before["rootfs"]) != (after["rootfs"])
        assert (before["packages"]) == (after["packages"])

    def test_staged_target_modules_compute_the_same_apk_recipes(
        self, registered_sources: RegistrationContext
    ) -> None:
        """A materialized target closure can import its build modules and calculate APK inputs."""
        names = ("fplinux-bluealsa", "fplinux-jpeg")
        expected = {
            name: alpine_recipes.alpine_package_recipe(
                name, "1" * 64, "d" * 64, registered_sources.repository
            )
            for name in names
        }
        snapshot = workspace_capture.workspace_snapshot(
            workspace_build_inputs.target_build_source_files("nokia-ta1618")
        )
        with mock.patch.object(workspace_staging, "ROOT", registered_sources.directory):
            staged = workspace_staging.stage_workspace_snapshot(snapshot)
        state = registered_sources.state(root=staged, names=names, selected=names)

        assert ({name: record["recipe"] for name, record in state["packages"].items()}) == (
            expected
        )
