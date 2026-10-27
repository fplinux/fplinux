# SPDX-License-Identifier: GPL-2.0-only
"""Alpine selection scenarios."""

from __future__ import annotations

from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.alpine import (
    lock as alpine_lock,
)
from fplinux_cli.alpine import (
    registration as alpine_registration,
)
from fplinux_cli.alpine import (
    selection as alpine_selection,
)
from fplinux_cli.alpine import (
    sources as alpine_sources,
)

from tests.small.alpine.fixtures import (
    alpine_source as alpine_source,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.small.alpine.fixtures import AlpineSourceContext


class AlpineSelectionTests:
    """Protect Alpine selection behavior with controlled temporary inputs."""

    @pytest.mark.parametrize(
        ("build_type", "expected", "expected_runtime"),
        [
            pytest.param("release", ("fplinux-package-a",), ("base-1-r0.apk",), id="release"),
            pytest.param(
                "debug",
                ("fplinux-debug-tools", "fplinux-package-a"),
                ("base-1-r0.apk", "strace-1-r0.apk", "libdw-1-r0.apk"),
                id="debug",
            ),
        ],
    )
    def test_build_type_selects_debug_tools_only_for_debug_rootfs(
        self,
        alpine_source: AlpineSourceContext,
        build_type: str,
        expected: tuple[str, ...],
        expected_runtime: tuple[str, ...],
    ) -> None:
        """Ordinary builds omit tracing tools, while debug builds install them."""
        alpine_source.write(
            "alpine/aports/fplinux-debug-tools/APKBUILD", b"pkgname=fplinux-debug-tools\n"
        )
        with mock.patch.object(alpine_registration, "COMMON_PACKAGES", ("fplinux-package-a",)):
            selected = alpine_selection.selected_packages(
                {"rootfs": {"packages": []}},
                {
                    "build_type": build_type,
                    "rootfs": {"base_packages": [], "packages": []},
                },
                alpine_source.root,
            )

        assert selected == expected
        lock = {
            "runtime": {
                "packages": ["base-1-r0.apk"],
                "additions": {"fplinux-debug-tools": ["strace-1-r0.apk", "libdw-1-r0.apk"]},
            }
        }
        assert alpine_lock.runtime_package_names(lock, selected) == expected_runtime

    def test_debug_tools_remain_rootfs_owned_with_optional_bundle_packages(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """Adding tracing tools preserves optional APK ownership and rejects overlap."""
        alpine_source.write(
            "alpine/aports/fplinux-debug-tools/APKBUILD", b"pkgname=fplinux-debug-tools\n"
        )
        platform = {
            "rootfs": {"packages": []},
            "bundle": {"packages": ["fplinux-package-b"]},
        }
        target = {
            "build_type": "debug",
            "rootfs": {"base_packages": [], "packages": []},
            "bundle": {"packages": []},
        }
        with mock.patch.object(alpine_registration, "COMMON_PACKAGES", ("fplinux-package-a",)):
            selected = alpine_selection.selected_packages(platform, target, alpine_source.root)

        assert alpine_selection.bundle_packages(
            platform, target, selected, alpine_source.root
        ) == ("fplinux-package-b",)
        with pytest.raises(SystemExit, match="both rootfs-selected and bundle-published"):
            alpine_selection.bundle_packages(
                {**platform, "bundle": {"packages": ["fplinux-debug-tools"]}},
                target,
                selected,
                alpine_source.root,
            )

    def test_debug_tool_changes_invalidate_only_the_selected_rootfs_recipe(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A tracing package edit changes only the selected debug rootfs recipe."""
        debug_aport = alpine_source.write(
            "alpine/aports/fplinux-debug-tools/APKBUILD", b"pkgname=fplinux-debug-tools\n"
        )
        platform: dict[str, object] = {"rootfs": {"packages": []}}
        target: dict[str, object] = {"rootfs": {"base_packages": [], "packages": []}}
        with (
            mock.patch.object(alpine_registration, "COMMON_PACKAGES", ("fplinux-package-a",)),
            mock.patch.object(alpine_registration, "LOCAL_BUILD_DEPENDENCIES", {}),
            mock.patch.object(alpine_registration, "SHARED_APORT_SOURCES", {}),
        ):
            release_packages = alpine_selection.selected_packages(
                platform, {**target, "build_type": "release"}, alpine_source.root
            )
            debug_packages = alpine_selection.selected_packages(
                platform, {**target, "build_type": "debug"}, alpine_source.root
            )
            release_recipe = alpine_source.recipe(packages=release_packages)
            debug_recipe = alpine_source.recipe(packages=debug_packages)
            debug_aport.write_bytes(b'pkgname=fplinux-debug-tools\npkgdesc="Tracing tools"\n')

            assert debug_recipe != release_recipe
            assert alpine_source.recipe(packages=debug_packages) != debug_recipe
            assert alpine_source.recipe(packages=release_packages) == release_recipe

    def test_selection_combines_common_and_platform_ownership(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """Common and platform ownership contribute one canonical rootfs set."""
        third = "fplinux-package-c"
        alpine_source.write(f"alpine/aports/{third}/APKBUILD", f"pkgname={third}\n".encode())
        with mock.patch.object(
            alpine_registration, "COMMON_PACKAGES", (alpine_source.packages[0],)
        ):
            selected = alpine_selection.selected_packages(
                {"rootfs": {"packages": [alpine_source.packages[1], third]}},
                {"rootfs": {"base_packages": [], "packages": []}},
                alpine_source.root,
            )
        assert (selected) == ((*alpine_source.packages, third))

    def test_build_order_prepares_transitive_libraries_before_consumers(self) -> None:
        """A library that consumes another local library sees it in the sysroot."""
        dependencies = {
            "fplinux-app": ("fplinux-a-library",),
            "fplinux-a-library": ("fplinux-z-library",),
        }
        with mock.patch.object(alpine_registration, "LOCAL_BUILD_DEPENDENCIES", dependencies):
            order = alpine_selection.aport_build_order(("fplinux-app", "fplinux-a-library"))
            libraries = alpine_selection.local_build_dependencies(("fplinux-app",))
        assert (set(order)) == ({"fplinux-app", "fplinux-a-library", "fplinux-z-library"})
        assert (len(order)) == (3)
        assert (order.index("fplinux-z-library")) < (order.index("fplinux-a-library"))
        assert (order.index("fplinux-a-library")) < (order.index("fplinux-app"))
        assert (set(libraries)) == ({"fplinux-a-library", "fplinux-z-library"})

    def test_selection_composes_target_base_with_profile_delta(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A profile adds its packages while retaining the target's base selection."""
        target_package = "fplinux-package-c"
        profile_package = "fplinux-package-d"
        alpine_source.write(
            f"alpine/aports/{target_package}/APKBUILD", f"pkgname={target_package}\n".encode()
        )
        alpine_source.write(
            f"alpine/aports/{profile_package}/APKBUILD", f"pkgname={profile_package}\n".encode()
        )
        with mock.patch.object(
            alpine_registration, "COMMON_PACKAGES", (alpine_source.packages[0],)
        ):
            selected = alpine_selection.selected_packages(
                {"rootfs": {"packages": [alpine_source.packages[1]]}},
                {
                    "rootfs": {
                        "base_packages": [target_package],
                        "packages": [profile_package],
                    }
                },
                alpine_source.root,
            )
        assert selected == (*alpine_source.packages, target_package, profile_package)

    def test_runtime_addition_is_selected_only_with_its_local_package(self) -> None:
        """A profile-only Alpine closure is unrelated to ordinary rootfs builds."""
        lock = {
            "runtime": {
                "packages": ["base-1-r0.apk"],
                "additions": {"fplinux-feature": ["feature-dependency-1-r0.apk"]},
            }
        }

        assert (alpine_lock.runtime_package_names(lock, ())) == (("base-1-r0.apk",))
        assert (alpine_lock.runtime_package_names(lock, ("fplinux-feature",))) == (
            ("base-1-r0.apk", "feature-dependency-1-r0.apk")
        )

    def test_optional_runtime_delivery_excludes_rootfs_and_unselected_dependencies(self) -> None:
        """Optional apps share extra APKs without bundling installed or unrelated closures."""
        lock = {
            "runtime": {
                "packages": ["base-1-r0.apk"],
                "additions": {
                    "fplinux-core": ["shared-2-r0.apk"],
                    "fplinux-feature": ["shared-2-r0.apk", "feature-runtime-3-r1.apk"],
                    "fplinux-other": ["feature-runtime-3-r1.apk", "other-runtime-4-r0.apk"],
                    "fplinux-unselected": ["unused-5-r0.apk"],
                },
            }
        }

        assert alpine_lock.bundle_runtime_packages(
            lock, ("fplinux-core",), ("fplinux-feature", "fplinux-other")
        ) == {
            "feature-runtime": "feature-runtime-3-r1.apk",
            "other-runtime": "other-runtime-4-r0.apk",
        }
        assert alpine_lock.bundle_runtime_packages(
            lock, ("fplinux-core", "fplinux-feature"), ("fplinux-other",)
        ) == {"other-runtime": "other-runtime-4-r0.apk"}
        assert alpine_lock.bundle_runtime_packages(lock, ("fplinux-core",), ()) == {}

    def test_bundle_subpackages_use_existing_aports_without_selecting_their_parents(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """Optional child APK names remain distinct from the preinstalled producer packages."""
        for producer in ("fplinux-alsa-lib", "fplinux-bash", "fplinux-ncurses"):
            alpine_source.write(
                f"alpine/aports/{producer}/APKBUILD", f"pkgname={producer}\n".encode()
            )
        optional = (
            "fplinux-alsa-lib-card-profiles",
            "fplinux-bash-loadables",
            "fplinux-ncurses-curses",
        )

        actual = alpine_selection.bundle_packages(
            {"bundle": {"packages": list(optional)}},
            {"bundle": {"packages": []}},
            ("fplinux-alsa-lib", "fplinux-bash", "fplinux-ncurses"),
            alpine_source.root,
        )

        assert (actual) == (optional)

    @pytest.mark.parametrize(
        ("rootfs_packages", "expected"),
        [
            pytest.param(
                ("fplinux-package-a",),
                ("fplinux-data-child", "fplinux-package-b", "fplinux-runtime"),
                id="runtime-data-is-optional",
            ),
            pytest.param(
                ("fplinux-package-a", "fplinux-data-child"),
                ("fplinux-package-b", "fplinux-runtime"),
                id="installed-runtime-data-stays-rootfs-owned",
            ),
        ],
    )
    def test_bundle_delivers_transitive_local_runtime_dependencies(
        self,
        alpine_source: AlpineSourceContext,
        rootfs_packages: tuple[str, ...],
        expected: tuple[str, ...],
    ) -> None:
        """Deliver an optional app's required data without its producer or build-only APKs."""
        alpine_source.write(
            "alpine/aports/fplinux-package-b/APKBUILD",
            b'pkgname=fplinux-package-b\ndepends="fplinux-package-a>=1 fplinux-runtime"\n',
        )
        alpine_source.write(
            "alpine/aports/fplinux-runtime/APKBUILD",
            b'pkgname=fplinux-runtime\ndepends="fplinux-data-child=2-r0 external-runtime"\n',
        )
        alpine_source.write(
            "alpine/aports/fplinux-data/APKBUILD",
            b'pkgname=fplinux-data\ndepends="fplinux-build-only"\n',
        )
        alpine_source.write(
            "alpine/aports/fplinux-build-only/APKBUILD", b"pkgname=fplinux-build-only\n"
        )
        with (
            mock.patch.dict(
                alpine_registration.SUBPACKAGE_APORTS, {"fplinux-data-child": "fplinux-data"}
            ),
            mock.patch.dict(
                alpine_registration.LOCAL_BUILD_DEPENDENCIES,
                {"fplinux-package-b": ("fplinux-build-only",)},
            ),
        ):
            actual = alpine_selection.bundle_packages(
                {"bundle": {"packages": ["fplinux-package-b"]}},
                {"bundle": {"packages": []}},
                rootfs_packages,
                alpine_source.root,
            )

        assert actual == expected

    def test_bundle_selection_does_not_resolve_unrelated_external_source_pins(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """Runtime names need no external source record; full materialization still requires it."""
        alpine_source.write(
            "alpine/aports/fplinux-package-b/APKBUILD",
            b"pkgname=fplinux-package-b\n"
            b"pkgver=@version:optional_original@\n"
            b"pkgrel=0\n"
            b'depends="$pkgname-helper=$pkgver-r$pkgrel"\n'
            b'source="@source:optional_original@"\n',
        )
        alpine_source.write(
            "alpine/aports/fplinux-package-b-helper/APKBUILD",
            b"pkgname=fplinux-package-b-helper\n",
        )

        actual = alpine_selection.bundle_packages(
            {"bundle": {"packages": ["fplinux-package-b"]}},
            {"bundle": {"packages": []}},
            ("fplinux-package-a",),
            alpine_source.root,
        )

        assert actual == ("fplinux-package-b", "fplinux-package-b-helper")
        with pytest.raises(SystemExit, match="source lock entry is missing: optional_original"):
            alpine_sources.aport_declaration(alpine_source.root, "fplinux-package-b")

    def test_package_cannot_be_selected_and_bundle_published(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """One package cannot be both installed and published separately."""
        with pytest.raises(SystemExit, match="both rootfs-selected and bundle-published"):
            alpine_selection.bundle_packages(
                {"bundle": {"packages": [alpine_source.packages[0]]}},
                {"bundle": {"packages": []}},
                alpine_source.packages,
                alpine_source.root,
            )

    def test_profile_preinstall_removes_only_its_platform_optional_package(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A profile dependency is installed, while unrelated components remain optional."""
        profile = {
            "rootfs": {
                "base_packages": [],
                "packages": ["fplinux-package-a"],
            },
            "bundle": {"packages": []},
        }
        platform = {"bundle": {"packages": ["fplinux-package-a", "fplinux-package-b"]}}

        assert (
            alpine_selection.bundle_packages(
                platform, profile, ("fplinux-package-a",), alpine_source.root
            )
        ) == (("fplinux-package-b",))
        with pytest.raises(SystemExit, match="both rootfs-selected and bundle-published"):
            alpine_selection.bundle_packages(
                {"bundle": {"packages": []}},
                {**profile, "bundle": {"packages": ["fplinux-package-a"]}},
                ("fplinux-package-a",),
                alpine_source.root,
            )
        with pytest.raises(SystemExit, match="owned by both platform and target"):
            alpine_selection.bundle_packages(
                platform,
                {**profile, "bundle": {"packages": ["fplinux-package-a"]}},
                ("fplinux-package-a",),
                alpine_source.root,
            )

    def test_bundle_selection_rejects_duplicate_platform_and_target_ownership(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A bundle package has one declarative owner, just like a rootfs package."""
        with pytest.raises(SystemExit, match="owned by both platform and target"):
            alpine_selection.bundle_packages(
                {"bundle": {"packages": [alpine_source.packages[0]]}},
                {"bundle": {"packages": [alpine_source.packages[0]]}},
                (),
                alpine_source.root,
            )

    def test_selection_rejects_duplicate_ownership(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """One package cannot be owned by both common and platform layers."""
        with (
            mock.patch.object(
                alpine_registration, "COMMON_PACKAGES", (alpine_source.packages[0],)
            ),
            pytest.raises(SystemExit, match="owned by both common and platform"),
        ):
            alpine_selection.selected_packages(
                {"rootfs": {"packages": [alpine_source.packages[0]]}},
                {"rootfs": {"base_packages": [], "packages": []}},
                alpine_source.root,
            )

    def test_profile_rootfs_rejects_duplicate_additions(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A profile cannot repeat a package already owned by the base rootfs."""
        with (
            mock.patch.object(
                alpine_registration, "COMMON_PACKAGES", (alpine_source.packages[0],)
            ),
            pytest.raises(SystemExit, match="duplicate base ownership"),
        ):
            alpine_selection.selected_packages(
                {"rootfs": {"packages": [alpine_source.packages[1]]}},
                {
                    "rootfs": {
                        "base_packages": [],
                        "packages": [alpine_source.packages[1]],
                    }
                },
                alpine_source.root,
            )

    def test_lock_requires_every_selected_artifact_to_be_declared(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """Reject a runtime/sysroot package without its exact artifact record."""
        lock = alpine_lock.load_alpine_lock(alpine_source.root)
        assert (lock["arch"]) == ("armv7")
        path = alpine_source.root / "alpine.lock.toml"
        text = path.read_text().replace(
            'packages = ["musl-dev-1-r0.apk"]',
            'packages = ["missing.apk"]',
        )
        path.write_text(text)
        with pytest.raises(SystemExit, match="has no locked artifact"):
            alpine_lock.load_alpine_lock(alpine_source.root)

    def test_lock_rejects_an_unknown_field(self, alpine_source: AlpineSourceContext) -> None:
        """The exact lock shape rejects unrecognized metadata."""
        path = alpine_source.root / "alpine.lock.toml"
        path.write_text('unexpected = "value"\n' + path.read_text())
        with pytest.raises(SystemExit, match="invalid Alpine lock"):
            alpine_lock.load_alpine_lock(alpine_source.root)

    @pytest.mark.parametrize(
        "consumer",
        [
            pytest.param("fplinux-brightness-ui", id="brightness-ui"),
            pytest.param("fplinux-showcase", id="showcase"),
        ],
    )
    @pytest.mark.parametrize(
        ("base", "profile", "selected_font"),
        [
            pytest.param((), (), None, id="missing-font"),
            pytest.param(
                ("fplinux-font-terminus-6x12", "fplinux-font-terminus-8x16"),
                (),
                None,
                id="conflicting-base-fonts",
            ),
            pytest.param(
                ("fplinux-font-terminus-6x12",),
                ("fplinux-font-terminus-8x16",),
                None,
                id="conflicting-profile-font",
            ),
            pytest.param(
                ("fplinux-font-terminus-6x12",),
                (),
                "fplinux-font-terminus-6x12",
                id="small-font",
            ),
            pytest.param(
                ("fplinux-font-terminus-8x16",),
                (),
                "fplinux-font-terminus-8x16",
                id="large-font",
            ),
        ],
    )
    def test_text_rootfs_requires_one_font_payload(
        self,
        alpine_source: AlpineSourceContext,
        *,
        consumer: str,
        base: tuple[str, ...],
        profile: tuple[str, ...],
        selected_font: str | None,
    ) -> None:
        """Text applications cannot start with no default font or competing defaults."""
        alpine_source.write(
            "alpine/aports/fplinux-font-terminus/APKBUILD", b"pkgname=fplinux-font-terminus\n"
        )
        alpine_source.write(f"alpine/aports/{consumer}/APKBUILD", f"pkgname={consumer}\n".encode())
        target = {
            "rootfs": {
                "base_packages": list(base),
                "packages": list(profile),
            }
        }
        with mock.patch.object(alpine_registration, "COMMON_PACKAGES", (consumer,)):
            if selected_font is None:
                with pytest.raises(SystemExit, match="exactly one Terminus font size"):
                    alpine_selection.selected_packages(
                        {"rootfs": {"packages": []}}, target, alpine_source.root
                    )
            else:
                selected = alpine_selection.selected_packages(
                    {"rootfs": {"packages": []}}, target, alpine_source.root
                )
                assert (set(selected)) == ({consumer, selected_font})
