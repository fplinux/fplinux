# SPDX-License-Identifier: GPL-2.0-only
"""Alpine recipes scenarios."""

from __future__ import annotations

import py_compile
import shutil
import unittest
from unittest import mock

from fplinux_cli.alpine import (
    aports as alpine_aports,
)
from fplinux_cli.alpine import (
    recipes as alpine_recipes,
)
from fplinux_cli.alpine import (
    registration as alpine_registration,
)
from fplinux_cli.alpine import (
    rootfs_state as alpine_rootfs_state,
)
from fplinux_cli.alpine import (
    selection as alpine_selection,
)

from tests import ROOT
from tests.small.alpine import fixtures


class AlpineRecipesTests(fixtures.AlpineSourceFixture):
    """Protect Alpine recipes behavior with controlled temporary inputs."""

    def test_selected_aport_bytes_change_the_rootfs_recipe(self) -> None:
        """A selected package-source edit invalidates the shared rootfs."""
        first = self._recipe()
        self.aport.write_bytes(f"pkgname={self.packages[0]}\npkgrel=1\n".encode())
        second = self._recipe()
        self.assertNotEqual(first, second)
        self.assertEqual(second, self._recipe())

    def test_kernel_only_builder_bytes_do_not_change_alpine_recipes(self) -> None:
        """Kernel-only builder edits cannot invalidate the Alpine rootfs or APK slots."""
        rootfs_before = self._recipe()
        package_before = alpine_recipes.alpine_package_recipe(
            self.packages[0], "1" * 64, self.signing_key, self.root
        )
        self._write(
            "scripts/fplinux_cli/build/kernel/compile.py", b"kernel implementation changed\n"
        )
        self.assertEqual(rootfs_before, self._recipe())
        self.assertEqual(
            package_before,
            alpine_recipes.alpine_package_recipe(
                self.packages[0], "1" * 64, self.signing_key, self.root
            ),
        )

    def test_aport_python_bytecode_preserves_receipt_and_package_recipe(self) -> None:
        """Compiling a helper is unrelated; changing its source invalidates its artifacts."""
        helper = self._write("alpine/aports/fplinux-package-a/helper.py", b"VALUE = 42\n")
        rootfs_recipe = self._recipe()
        package_recipe = alpine_recipes.alpine_package_recipe(
            self.packages[0], "1" * 64, self.signing_key, self.root
        )
        output = self.root / "cached-rootfs"
        output.mkdir()
        (output / "rootfs.cpio").write_bytes(b"logical composition\n")
        alpine_rootfs_state.write_receipt(output, rootfs_recipe)
        self.assertTrue(alpine_rootfs_state.receipt_matches(output, self._recipe()))

        py_compile.compile(str(helper), doraise=True)
        self.assertTrue(alpine_rootfs_state.receipt_matches(output, self._recipe()))
        self.assertEqual(
            package_recipe,
            alpine_recipes.alpine_package_recipe(
                self.packages[0], "1" * 64, self.signing_key, self.root
            ),
        )

        helper.write_bytes(b"VALUE = 43\n")
        self.assertFalse(alpine_rootfs_state.receipt_matches(output, self._recipe()))
        self.assertNotEqual(
            package_recipe,
            alpine_recipes.alpine_package_recipe(
                self.packages[0], "1" * 64, self.signing_key, self.root
            ),
        )

    def test_alpine_preparation_changes_invalidate_rootfs_and_package_recipes(self) -> None:
        """Build and shared extraction changes invalidate the artifacts they prepare."""
        for source in (
            "alpine/aports.py",
            "alpine/packages.py",
            "common.py",
            "build/inputs.py",
            "build/process.py",
            "build/sources.py",
        ):
            with self.subTest(source=source):
                rootfs_before = self._recipe()
                package_before = alpine_recipes.alpine_package_recipe(
                    self.packages[0], "1" * 64, self.signing_key, self.root
                )
                self._write(f"scripts/fplinux_cli/{source}", b"preparation changed\n")
                self.assertNotEqual(rootfs_before, self._recipe())
                self.assertNotEqual(
                    package_before,
                    alpine_recipes.alpine_package_recipe(
                        self.packages[0], "1" * 64, self.signing_key, self.root
                    ),
                )

    def test_build_environment_bytes_change_alpine_recipes(self) -> None:
        """A shared deterministic-environment edit invalidates rootfs and APK slots."""
        rootfs_before = self._recipe()
        package_before = alpine_recipes.alpine_package_recipe(
            self.packages[0], "1" * 64, self.signing_key, self.root
        )

        self._write("scripts/fplinux_cli/build/environment.py", b"changed environment\n")

        self.assertNotEqual(rootfs_before, self._recipe())
        self.assertNotEqual(
            package_before,
            alpine_recipes.alpine_package_recipe(
                self.packages[0], "1" * 64, self.signing_key, self.root
            ),
        )

    def test_aport_edit_invalidates_only_its_package_recipe(self) -> None:
        """An ordinary local edit does not rebuild unrelated FPLinux APKs."""
        before = {
            name: alpine_recipes.alpine_package_recipe(
                name,
                "1" * 64,
                self.signing_key,
                self.root,
            )
            for name in self.packages
        }
        changed_aport = self.root / f"alpine/aports/{self.packages[1]}/APKBUILD"
        changed_aport.write_bytes(f"pkgname={self.packages[1]}\npkgrel=1\n".encode())
        after = {
            name: alpine_recipes.alpine_package_recipe(
                name,
                "1" * 64,
                self.signing_key,
                self.root,
            )
            for name in self.packages
        }

        self.assertNotEqual(before[self.packages[1]], after[self.packages[1]])
        for name in set(self.packages) - {self.packages[1]}:
            self.assertEqual(before[name], after[name])

    def test_declared_shared_source_is_causal_only_for_its_consumer(self) -> None:
        """A mapped source invalidates its consumer without relabeling another APK."""
        mapping = {self.packages[0]: ("alpine/shared/shared.c",)}
        with mock.patch.object(alpine_registration, "SHARED_APORT_SOURCES", mapping):
            before = {
                name: alpine_recipes.alpine_package_recipe(
                    name, "1" * 64, self.signing_key, self.root
                )
                for name in self.packages
            }
            rootfs_before = self._recipe()
            self.shared_source.write_bytes(b"int shared = 1;\n")
            after = {
                name: alpine_recipes.alpine_package_recipe(
                    name, "1" * 64, self.signing_key, self.root
                )
                for name in self.packages
            }
            rootfs_after = self._recipe()

        self.assertNotEqual(rootfs_before, rootfs_after)
        self.assertNotEqual(before[self.packages[0]], after[self.packages[0]])
        self.assertEqual(before[self.packages[1]], after[self.packages[1]])

    def test_font_source_changes_only_text_consumers_and_is_staged_for_each(self) -> None:
        """A font-reader edit invalidates its three consumers and reaches their aport stages."""
        consumers = ("fplinux-terminal", "fplinux-brightness-ui", "fplinux-showcase")
        unrelated = "fplinux-present"
        for name in (
            *consumers,
            unrelated,
            "fplinux-font-terminus",
            "fplinux-libdrm",
            "fplinux-libtsm",
            "fplinux-libxkbcommon",
        ):
            self._write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
        repository = ROOT
        for directory in ("lib/fplinux", "include/fplinux"):
            shutil.copytree(repository / directory, self.root / directory)
        font_source = self._write("lib/fplinux/fplinux-font.c", b"fixture original font reader\n")
        self._write("include/fplinux/fplinux-font.h", b"fixture font interface\n")
        before = {
            name: alpine_recipes.alpine_package_recipe(name, "1" * 64, self.signing_key, self.root)
            for name in (*consumers, unrelated)
        }

        font_source.write_bytes(b"fixture changed font reader\n")

        for name in consumers:
            with self.subTest(consumer=name):
                after = alpine_recipes.alpine_package_recipe(
                    name, "1" * 64, self.signing_key, self.root
                )
                self.assertNotEqual(before[name], after)
                stage = self.root / "stages" / name
                alpine_aports.materialize_aport_sources(name, self.root, stage)
                self.assertEqual(
                    (stage / "fplinux-font.c").read_bytes(), b"fixture changed font reader\n"
                )
                self.assertEqual(
                    (stage / "fplinux-font.h").read_bytes(), b"fixture font interface\n"
                )
        self.assertEqual(
            before[unrelated],
            alpine_recipes.alpine_package_recipe(unrelated, "1" * 64, self.signing_key, self.root),
        )

    def test_library_changes_invalidate_consumer_recipes_only(self) -> None:
        """Library source and configuration changes invalidate linked APKs and rootfs inputs."""
        packages = (
            "fplinux-font-terminus",
            "fplinux-libdrm",
            "fplinux-libtsm",
            "fplinux-libxkbcommon",
            "fplinux-present",
            "fplinux-terminal",
        )
        for name in packages:
            self._write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
            for source in alpine_selection.shared_aport_sources(name, self.root):
                self._write(source.relative_to(self.root).as_posix(), b"shared source\n")

        cases = (
            ("fplinux-libdrm", "library.c", {"fplinux-present", "fplinux-terminal"}),
            ("fplinux-libtsm", "APKBUILD", {"fplinux-terminal"}),
            ("fplinux-libxkbcommon", "APKBUILD", {"fplinux-terminal"}),
        )
        for library, filename, consumers in cases:
            with self.subTest(library=library):
                before = {
                    name: alpine_recipes.alpine_package_recipe(
                        name, "1" * 64, self.signing_key, self.root
                    )
                    for name in packages
                }
                rootfs_before = self._recipe(packages=("fplinux-terminal",))
                self._write(f"alpine/aports/{library}/{filename}", b"changed library input\n")
                for name in packages:
                    after = alpine_recipes.alpine_package_recipe(
                        name, "1" * 64, self.signing_key, self.root
                    )
                    if name in {library, *consumers}:
                        self.assertNotEqual(before[name], after, name)
                    else:
                        self.assertEqual(before[name], after, name)
                self.assertNotEqual(rootfs_before, self._recipe(packages=("fplinux-terminal",)))

    def test_unselected_aport_is_not_causal(self) -> None:
        """Files outside the selected package set cannot invalidate its rootfs."""
        first = self._recipe()
        self._write("alpine/aports/not-production/APKBUILD", b"pkgrel=9\n")
        self.assertEqual(first, self._recipe())

    def test_package_set_is_causal(self) -> None:
        """Removing one otherwise valid package selects a different rootfs."""
        self.assertNotEqual(self._recipe(), self._recipe(packages=self.packages[:1]))

    def test_container_runtime_recipe_is_causal(self) -> None:
        """Changing the build environment invalidates the rootfs recipe."""
        self.assertNotEqual(self._recipe("1" * 64), self._recipe("2" * 64))

    def test_display_brightness_table_changes_rootfs_recipe(self) -> None:
        """A target brightness table change selects another rootfs generation."""
        original = {"backlight": "screen-backlight", "levels": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]}
        changed = {"backlight": "screen-backlight", "levels": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11]}
        self.assertNotEqual(self._recipe(), self._recipe(display_brightness=original))
        self.assertNotEqual(
            self._recipe(display_brightness=original),
            self._recipe(display_brightness=changed),
        )

    def test_package_signing_key_is_causal(self) -> None:
        """A different persistent abuild key must produce a different recipe."""
        self.assertNotEqual(self._recipe(signing_key="d" * 64), self._recipe(signing_key="e" * 64))


if __name__ == "__main__":
    unittest.main()
