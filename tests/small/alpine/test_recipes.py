# SPDX-License-Identifier: GPL-2.0-only
"""Alpine recipes scenarios."""

from __future__ import annotations

import py_compile
import shutil
from typing import TYPE_CHECKING
from unittest import mock

import pytest
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
from tests.small.alpine.fixtures import (
    alpine_source as alpine_source,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.small.alpine.fixtures import AlpineSourceContext


class AlpineRecipesTests:
    """Protect Alpine recipes behavior with controlled temporary inputs."""

    def test_selected_aport_bytes_change_the_rootfs_recipe(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A selected package-source edit invalidates the shared rootfs."""
        first = alpine_source.recipe()
        alpine_source.aport.write_bytes(
            f"pkgname={alpine_source.packages[0]}\npkgrel=1\n".encode()
        )
        second = alpine_source.recipe()
        assert (first) != (second)
        assert (second) == (alpine_source.recipe())

    def test_kernel_only_builder_bytes_do_not_change_alpine_recipes(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """Kernel-only builder edits cannot invalidate the Alpine rootfs or APK slots."""
        rootfs_before = alpine_source.recipe()
        package_before = alpine_recipes.alpine_package_recipe(
            alpine_source.packages[0], "1" * 64, alpine_source.signing_key, alpine_source.root
        )
        alpine_source.write(
            "scripts/fplinux_cli/build/kernel/compile.py", b"kernel implementation changed\n"
        )
        assert (rootfs_before) == (alpine_source.recipe())
        assert (package_before) == (
            alpine_recipes.alpine_package_recipe(
                alpine_source.packages[0], "1" * 64, alpine_source.signing_key, alpine_source.root
            )
        )

    def test_aport_python_bytecode_preserves_receipt_and_package_recipe(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """Compiling a helper is unrelated; changing its source invalidates its artifacts."""
        helper = alpine_source.write("alpine/aports/fplinux-package-a/helper.py", b"VALUE = 42\n")
        rootfs_recipe = alpine_source.recipe()
        package_recipe = alpine_recipes.alpine_package_recipe(
            alpine_source.packages[0], "1" * 64, alpine_source.signing_key, alpine_source.root
        )
        output = alpine_source.root / "cached-rootfs"
        output.mkdir()
        (output / "rootfs.cpio").write_bytes(b"logical composition\n")
        alpine_rootfs_state.write_receipt(output, rootfs_recipe)
        assert alpine_rootfs_state.receipt_matches(output, alpine_source.recipe())

        py_compile.compile(str(helper), doraise=True)
        assert alpine_rootfs_state.receipt_matches(output, alpine_source.recipe())
        assert (package_recipe) == (
            alpine_recipes.alpine_package_recipe(
                alpine_source.packages[0], "1" * 64, alpine_source.signing_key, alpine_source.root
            )
        )

        helper.write_bytes(b"VALUE = 43\n")
        assert not (alpine_rootfs_state.receipt_matches(output, alpine_source.recipe()))
        assert (package_recipe) != (
            alpine_recipes.alpine_package_recipe(
                alpine_source.packages[0], "1" * 64, alpine_source.signing_key, alpine_source.root
            )
        )

    @pytest.mark.parametrize(
        "source",
        [
            pytest.param("alpine/aports.py", id="aport-preparation"),
            pytest.param("alpine/packages.py", id="package-preparation"),
            pytest.param("common.py", id="shared-extraction"),
            pytest.param("build/inputs.py", id="build-inputs"),
            pytest.param("build/process.py", id="build-process"),
            pytest.param("build/sources.py", id="build-sources"),
            pytest.param("environment/downloads.py", id="verified-transfer"),
            pytest.param("manifests/values.py", id="sha-validation"),
        ],
    )
    def test_alpine_preparation_changes_invalidate_rootfs_and_package_recipes(
        self, alpine_source: AlpineSourceContext, source: str
    ) -> None:
        """Build and shared extraction changes invalidate the artifacts they prepare."""
        rootfs_before = alpine_source.recipe()
        package_before = alpine_recipes.alpine_package_recipe(
            alpine_source.packages[0], "1" * 64, alpine_source.signing_key, alpine_source.root
        )
        output = alpine_source.root / "cached-rootfs"
        output.mkdir()
        (output / "rootfs.cpio").write_bytes(b"logical composition\n")
        alpine_rootfs_state.write_receipt(output, rootfs_before)
        assert alpine_rootfs_state.receipt_matches(output, alpine_source.recipe())
        alpine_source.write("README.md", b"unrelated documentation\n")
        assert alpine_rootfs_state.receipt_matches(output, alpine_source.recipe())
        assert package_before == alpine_recipes.alpine_package_recipe(
            alpine_source.packages[0], "1" * 64, alpine_source.signing_key, alpine_source.root
        )
        alpine_source.write(f"scripts/fplinux_cli/{source}", b"preparation changed\n")
        assert not alpine_rootfs_state.receipt_matches(output, alpine_source.recipe())
        assert (rootfs_before) != (alpine_source.recipe())
        assert (package_before) != (
            alpine_recipes.alpine_package_recipe(
                alpine_source.packages[0], "1" * 64, alpine_source.signing_key, alpine_source.root
            )
        )

    def test_build_environment_bytes_change_alpine_recipes(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A shared deterministic-environment edit invalidates rootfs and APK slots."""
        rootfs_before = alpine_source.recipe()
        package_before = alpine_recipes.alpine_package_recipe(
            alpine_source.packages[0], "1" * 64, alpine_source.signing_key, alpine_source.root
        )

        alpine_source.write("scripts/fplinux_cli/build/environment.py", b"changed environment\n")

        assert (rootfs_before) != (alpine_source.recipe())
        assert (package_before) != (
            alpine_recipes.alpine_package_recipe(
                alpine_source.packages[0], "1" * 64, alpine_source.signing_key, alpine_source.root
            )
        )

    def test_aport_edit_invalidates_only_its_package_recipe(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """An ordinary local edit does not rebuild unrelated FPLinux APKs."""
        before = {
            name: alpine_recipes.alpine_package_recipe(
                name,
                "1" * 64,
                alpine_source.signing_key,
                alpine_source.root,
            )
            for name in alpine_source.packages
        }
        changed_aport = alpine_source.root / f"alpine/aports/{alpine_source.packages[1]}/APKBUILD"
        changed_aport.write_bytes(f"pkgname={alpine_source.packages[1]}\npkgrel=1\n".encode())
        after = {
            name: alpine_recipes.alpine_package_recipe(
                name,
                "1" * 64,
                alpine_source.signing_key,
                alpine_source.root,
            )
            for name in alpine_source.packages
        }

        assert (before[alpine_source.packages[1]]) != (after[alpine_source.packages[1]])
        for name in set(alpine_source.packages) - {alpine_source.packages[1]}:
            assert (before[name]) == (after[name])

    def test_declared_shared_source_is_causal_only_for_its_consumer(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A mapped source invalidates its consumer without relabeling another APK."""
        mapping = {alpine_source.packages[0]: ("alpine/shared/shared.c",)}
        with mock.patch.object(alpine_registration, "SHARED_APORT_SOURCES", mapping):
            before = {
                name: alpine_recipes.alpine_package_recipe(
                    name, "1" * 64, alpine_source.signing_key, alpine_source.root
                )
                for name in alpine_source.packages
            }
            rootfs_before = alpine_source.recipe()
            alpine_source.shared_source.write_bytes(b"int shared = 1;\n")
            after = {
                name: alpine_recipes.alpine_package_recipe(
                    name, "1" * 64, alpine_source.signing_key, alpine_source.root
                )
                for name in alpine_source.packages
            }
            rootfs_after = alpine_source.recipe()

        assert (rootfs_before) != (rootfs_after)
        assert (before[alpine_source.packages[0]]) != (after[alpine_source.packages[0]])
        assert (before[alpine_source.packages[1]]) == (after[alpine_source.packages[1]])

    @pytest.mark.parametrize(
        "consumer",
        [
            pytest.param("fplinux-terminal", id="terminal"),
            pytest.param("fplinux-brightness-ui", id="brightness-ui"),
            pytest.param("fplinux-showcase", id="showcase"),
        ],
    )
    def test_font_source_changes_only_text_consumers_and_is_staged_for_each(
        self, alpine_source: AlpineSourceContext, consumer: str
    ) -> None:
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
            alpine_source.write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
        repository = ROOT
        for directory in ("lib/fplinux", "include/fplinux"):
            shutil.copytree(repository / directory, alpine_source.root / directory)
        font_source = alpine_source.write(
            "lib/fplinux/fplinux-font.c", b"fixture original font reader\n"
        )
        alpine_source.write("include/fplinux/fplinux-font.h", b"fixture font interface\n")
        before = {
            name: alpine_recipes.alpine_package_recipe(
                name, "1" * 64, alpine_source.signing_key, alpine_source.root
            )
            for name in (*consumers, unrelated)
        }

        font_source.write_bytes(b"fixture changed font reader\n")

        after = alpine_recipes.alpine_package_recipe(
            consumer, "1" * 64, alpine_source.signing_key, alpine_source.root
        )
        assert (before[consumer]) != (after)
        stage = alpine_source.root / "stages" / consumer
        alpine_aports.materialize_aport_sources(consumer, alpine_source.root, stage)
        assert ((stage / "fplinux-font.c").read_bytes()) == (b"fixture changed font reader\n")
        assert ((stage / "fplinux-font.h").read_bytes()) == (b"fixture font interface\n")
        assert (before[unrelated]) == (
            alpine_recipes.alpine_package_recipe(
                unrelated, "1" * 64, alpine_source.signing_key, alpine_source.root
            )
        )

    @pytest.mark.parametrize(
        ("library", "filename", "consumers"),
        [
            pytest.param(
                "fplinux-libdrm",
                "library.c",
                ("fplinux-present", "fplinux-terminal"),
                id="drm-source",
            ),
            pytest.param("fplinux-libtsm", "APKBUILD", ("fplinux-terminal",), id="tsm-aport"),
            pytest.param(
                "fplinux-libxkbcommon", "APKBUILD", ("fplinux-terminal",), id="xkbcommon-aport"
            ),
        ],
    )
    def test_library_changes_invalidate_consumer_recipes_only(
        self,
        alpine_source: AlpineSourceContext,
        library: str,
        filename: str,
        consumers: tuple[str, ...],
    ) -> None:
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
            alpine_source.write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
            for source in alpine_selection.shared_aport_sources(name, alpine_source.root):
                alpine_source.write(
                    source.relative_to(alpine_source.root).as_posix(), b"shared source\n"
                )

        before = {
            name: alpine_recipes.alpine_package_recipe(
                name, "1" * 64, alpine_source.signing_key, alpine_source.root
            )
            for name in packages
        }
        rootfs_before = alpine_source.recipe(packages=("fplinux-terminal",))
        alpine_source.write(f"alpine/aports/{library}/{filename}", b"changed library input\n")
        for name in packages:
            after = alpine_recipes.alpine_package_recipe(
                name, "1" * 64, alpine_source.signing_key, alpine_source.root
            )
            if name in {library, *consumers}:
                assert (before[name]) != (after), name
            else:
                assert (before[name]) == (after), name
        assert (rootfs_before) != (alpine_source.recipe(packages=("fplinux-terminal",)))

    def test_unselected_aport_is_not_causal(self, alpine_source: AlpineSourceContext) -> None:
        """Files outside the selected package set cannot invalidate its rootfs."""
        first = alpine_source.recipe()
        alpine_source.write("alpine/aports/not-production/APKBUILD", b"pkgrel=9\n")
        assert (first) == (alpine_source.recipe())

    def test_package_set_is_causal(self, alpine_source: AlpineSourceContext) -> None:
        """Removing one otherwise valid package selects a different rootfs."""
        assert (alpine_source.recipe()) != (
            alpine_source.recipe(packages=alpine_source.packages[:1])
        )

    def test_container_runtime_recipe_is_causal(self, alpine_source: AlpineSourceContext) -> None:
        """Changing the build environment invalidates the rootfs recipe."""
        assert (alpine_source.recipe("1" * 64)) != (alpine_source.recipe("2" * 64))

    def test_display_brightness_table_changes_rootfs_recipe(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A target brightness table change selects another rootfs generation."""
        original = {"backlight": "screen-backlight", "levels": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]}
        changed = {"backlight": "screen-backlight", "levels": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11]}
        assert (alpine_source.recipe()) != (alpine_source.recipe(display_brightness=original))
        assert (alpine_source.recipe(display_brightness=original)) != (
            alpine_source.recipe(display_brightness=changed)
        )

    def test_package_signing_key_is_causal(self, alpine_source: AlpineSourceContext) -> None:
        """A different persistent abuild key must produce a different recipe."""
        assert (alpine_source.recipe(signing_key="d" * 64)) != (
            alpine_source.recipe(signing_key="e" * 64)
        )
