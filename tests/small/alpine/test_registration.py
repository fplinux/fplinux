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
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli.alpine import packages as alpine_packages
from fplinux_cli.alpine import recipes as alpine_recipes
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


class AlpineRegistrationTests(unittest.TestCase):
    """Registration edits preserve unrelated APK receipts in isolated source trees."""

    _STATE_COMMAND = """
import json
import sys
from pathlib import Path
sys.dont_write_bytecode = True
root = Path(sys.argv[1])
sys.path.insert(0, str(root / "scripts"))
from fplinux_cli.alpine import packages, recipes
packages.CACHE = root / ".cache"
names = json.loads(sys.argv[2])
selected = json.loads(sys.argv[3])
result = {
    "packages": {
        name: {
            "recipe": recipes.alpine_package_recipe(name, "1" * 64, "d" * 64),
            "cache_hit": packages._cached_aport_packages(
                name, "1" * 64, "d" * 64
            ) is not None,
        }
        for name in names
    },
    "rootfs": recipes.alpine_rootfs_recipe("1" * 64, "d" * 64, selected),
}
print(json.dumps(result))
"""

    def setUp(self) -> None:
        """Copy host modules and replace the container cache with an owned temporary path."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.repository = ROOT
        shutil.copytree(
            self.repository / "scripts/fplinux_cli",
            self.root / "scripts/fplinux_cli",
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        self.names = (
            "fplinux-package-a",
            "fplinux-package-b",
            "fplinux-library-c",
            "fplinux-library-d",
        )
        self.selected = self.names[:2]
        for name in self.names:
            self._write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
        self._write("alpine.lock.toml", b"fixture lock\n")
        self._write("alpine/abuild.conf", b"fixture abuild\n")
        self._write("alpine/ramroot-init.sh", b"#!/bin/sh\nexit 0\n")
        self.registration = self.root / "scripts/fplinux_cli/alpine/registration.py"

    def _write(self, relative: str, contents: bytes) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def _state(
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
                "-c",
                self._STATE_COMMAND,
                str(self.root if root is None else root),
                json.dumps(self.names if names is None else names),
                json.dumps(self.selected if selected is None else selected),
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        state = json.loads(result.stdout)
        self.assertIsInstance(state, dict)
        return dict(state)

    def _seed_receipts(self) -> dict[str, Any]:
        """Publish ordinary output receipts for fixture APKs containing real package metadata."""
        state = self._state()
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
        seeded = self._state()
        for name in self.names:
            self.assertTrue(seeded["packages"][name]["cache_hit"], name)
        return seeded

    def _edit_registration(self, before: str, after: str) -> None:
        """Change declarations in the copied source, never generated cache files."""
        source = self.registration.read_text(encoding="utf-8")
        self.registration.write_text(source.replace(before, after, 1), encoding="utf-8")

    def test_unrelated_registration_preserves_apk_recipe_and_receipt_hit(self) -> None:
        """A newly declared dependency rebuilds its consumer while another APK stays reusable."""
        before = self._seed_receipts()

        self._edit_registration(
            "LOCAL_BUILD_DEPENDENCIES = {\n",
            'LOCAL_BUILD_DEPENDENCIES = {\n    "fplinux-package-a": ("fplinux-library-c",),\n',
        )
        after = self._state()

        self.assertNotEqual(
            before["packages"][self.names[0]]["recipe"],
            after["packages"][self.names[0]]["recipe"],
        )
        self.assertFalse(after["packages"][self.names[0]]["cache_hit"])
        for name in self.names[1:]:
            self.assertEqual(before["packages"][name]["recipe"], after["packages"][name]["recipe"])
            self.assertTrue(after["packages"][name]["cache_hit"], name)

    def test_dependency_edges_invalidate_even_when_transitive_library_set_is_unchanged(
        self,
    ) -> None:
        """Moving a dependency edge changes the consuming APKs despite identical library bytes."""
        direct = '    "fplinux-package-a": ("fplinux-library-c", "fplinux-library-d"),\n'
        self._edit_registration(
            "LOCAL_BUILD_DEPENDENCIES = {\n", "LOCAL_BUILD_DEPENDENCIES = {\n" + direct
        )
        before = self._seed_receipts()

        self._edit_registration(
            direct,
            '    "fplinux-package-a": ("fplinux-library-c",),\n'
            '    "fplinux-library-c": ("fplinux-library-d",),\n',
        )
        after = self._state()

        for name in (self.names[0], self.names[2]):
            self.assertNotEqual(
                before["packages"][name]["recipe"], after["packages"][name]["recipe"]
            )
            self.assertFalse(after["packages"][name]["cache_hit"], name)
        for name in (self.names[1], self.names[3]):
            self.assertEqual(before["packages"][name]["recipe"], after["packages"][name]["recipe"])
            self.assertTrue(after["packages"][name]["cache_hit"], name)
        self.assertNotEqual(before["rootfs"], after["rootfs"])

    def test_transitive_dependency_and_shared_source_edits_revoke_consumer_receipts(self) -> None:
        """Library edits revoke transitive consumers while an unrelated APK remains reusable."""
        self._edit_registration(
            "LOCAL_BUILD_DEPENDENCIES = {\n",
            "LOCAL_BUILD_DEPENDENCIES = {\n"
            '    "fplinux-package-a": ("fplinux-library-c",),\n'
            '    "fplinux-library-c": ("fplinux-library-d",),\n',
        )
        shared = self._write("lib/fplinux/library-d.c", b"library shared input\n")
        self._edit_registration(
            "SHARED_APORT_SOURCES = {\n",
            'SHARED_APORT_SOURCES = {\n    "fplinux-library-d": ("lib/fplinux/library-d.c",),\n',
        )
        library = self.root / "alpine/aports/fplinux-library-d/APKBUILD"

        for source in (library, shared):
            with self.subTest(source=source.relative_to(self.root).as_posix()):
                before = self._seed_receipts()
                source.write_bytes(source.read_bytes() + b"changed input\n")
                after = self._state()
                for name in (self.names[0], self.names[2], self.names[3]):
                    self.assertNotEqual(
                        before["packages"][name]["recipe"], after["packages"][name]["recipe"]
                    )
                    self.assertFalse(after["packages"][name]["cache_hit"], name)
                unrelated = self.names[1]
                self.assertEqual(
                    before["packages"][unrelated]["recipe"],
                    after["packages"][unrelated]["recipe"],
                )
                self.assertTrue(after["packages"][unrelated]["cache_hit"])
                self.assertNotEqual(before["rootfs"], after["rootfs"])

    def test_equivalent_producer_aliases_and_dependency_order_preserve_receipts(self) -> None:
        """Equivalent declarations of the same producer graph do not rebuild its outputs."""
        self._edit_registration(
            "SUBPACKAGE_APORTS = {\n",
            'SUBPACKAGE_APORTS = {\n    "fplinux-library-c-extra": "fplinux-library-c",\n',
        )
        original = (
            '    "fplinux-package-a": '
            '("fplinux-library-c", "fplinux-library-c-extra", "fplinux-library-d"),\n'
        )
        self._edit_registration(
            "LOCAL_BUILD_DEPENDENCIES = {\n", "LOCAL_BUILD_DEPENDENCIES = {\n" + original
        )
        before = self._seed_receipts()

        self._edit_registration(
            original,
            '    "fplinux-package-a": ("fplinux-library-d", "fplinux-library-c"),\n',
        )
        self.assertEqual(before, self._state())

    def test_shared_source_owner_changes_revoke_affected_receipts(self) -> None:
        """Moving a shared input between producers changes recipes despite an unchanged union."""
        self._edit_registration(
            "LOCAL_BUILD_DEPENDENCIES = {\n",
            'LOCAL_BUILD_DEPENDENCIES = {\n    "fplinux-package-a": ("fplinux-library-c",),\n',
        )
        self._write("lib/fplinux/shared.c", b"same shared source\n")
        original = '    "fplinux-package-a": ("lib/fplinux/shared.c",),\n'
        self._edit_registration(
            "SHARED_APORT_SOURCES = {\n", "SHARED_APORT_SOURCES = {\n" + original
        )
        before = self._seed_receipts()

        self._edit_registration(original, '    "fplinux-library-c": ("lib/fplinux/shared.c",),\n')
        after = self._state()

        for name in (self.names[0], self.names[2]):
            self.assertNotEqual(
                before["packages"][name]["recipe"], after["packages"][name]["recipe"]
            )
            self.assertFalse(after["packages"][name]["cache_hit"], name)
        for name in (self.names[1], self.names[3]):
            self.assertEqual(before["packages"][name]["recipe"], after["packages"][name]["recipe"])
            self.assertTrue(after["packages"][name]["cache_hit"], name)
        self.assertNotEqual(before["rootfs"], after["rootfs"])

    def test_recipe_implementation_edit_revokes_all_apk_receipts(self) -> None:
        """Algorithm-module changes remain a global input for every APK."""
        before = self._seed_receipts()
        implementation = self.root / "scripts/fplinux_cli/alpine/recipes.py"
        implementation.write_text(
            implementation.read_text(encoding="utf-8") + "\n# Fixture algorithm-module edit.\n",
            encoding="utf-8",
        )
        after = self._state()

        for name in self.names:
            self.assertNotEqual(
                before["packages"][name]["recipe"], after["packages"][name]["recipe"]
            )
            self.assertFalse(after["packages"][name]["cache_hit"], name)
        self.assertNotEqual(before["rootfs"], after["rootfs"])

    def test_rootfs_selection_changes_composition_and_preserves_apk_receipts(self) -> None:
        """Changing only the selected composition reuses every unchanged APK."""
        before = self._seed_receipts()
        after = self._state(selected=self.selected[:1])
        self.assertNotEqual(before["rootfs"], after["rootfs"])
        self.assertEqual(before["packages"], after["packages"])

    def test_staged_target_modules_compute_the_same_apk_recipes(self) -> None:
        """A materialized target closure can import its build modules and calculate APK inputs."""
        names = ("fplinux-bluealsa", "fplinux-jpeg")
        expected = {
            name: alpine_recipes.alpine_package_recipe(name, "1" * 64, "d" * 64, self.repository)
            for name in names
        }
        snapshot = workspace_capture.workspace_snapshot(
            workspace_build_inputs.target_build_source_files("nokia-ta1618")
        )
        with mock.patch.object(workspace_staging, "ROOT", Path(self.temporary.name)):
            staged = workspace_staging.stage_workspace_snapshot(snapshot)
        state = self._state(root=staged, names=names, selected=names)

        self.assertEqual(
            {name: record["recipe"] for name, record in state["packages"].items()}, expected
        )


if __name__ == "__main__":
    unittest.main()
