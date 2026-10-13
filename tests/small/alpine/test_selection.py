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

from tests.small.alpine.fixtures import (
    alpine_source as alpine_source,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.small.alpine.fixtures import AlpineSourceContext


class AlpineSelectionTests:
    """Protect Alpine selection behavior with controlled temporary inputs."""

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
                {"rootfs": {"base_packages": [], "packages": [], "exclude_packages": []}},
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
        """A normalized target base and profile delta retain only unexcluded packages."""
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
                        "exclude_packages": [target_package],
                    }
                },
                alpine_source.root,
            )
        assert (selected) == ((*alpine_source.packages, profile_package))

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
                "exclude_packages": [],
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
                {"rootfs": {"base_packages": [], "packages": [], "exclude_packages": []}},
                alpine_source.root,
            )

    def test_selected_profile_can_replace_platform_rootfs_packages(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A profile delta removes declared base packages and adds a distinct package."""
        extra = "fplinux-package-c"
        alpine_source.write(f"alpine/aports/{extra}/APKBUILD", f"pkgname={extra}\n".encode())
        with mock.patch.object(
            alpine_registration, "COMMON_PACKAGES", (alpine_source.packages[0],)
        ):
            selected = alpine_selection.selected_packages(
                {"rootfs": {"packages": [alpine_source.packages[1]]}},
                {
                    "rootfs": {
                        "base_packages": [],
                        "packages": [extra],
                        "exclude_packages": [alpine_source.packages[1]],
                    }
                },
                alpine_source.root,
            )
        assert (selected) == ((alpine_source.packages[0], extra))

    def test_profile_can_exclude_gadget_input_stack_and_retain_terminal(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A host-only rootfs removes gadget-only input without losing the terminal."""
        common = ("fplinux-base", "fplinux-terminal", "fplinux-input")
        platform = ("fplinux-usb-gadget", "fplinux-ssh")
        for package in (*common, *platform):
            alpine_source.write(
                f"alpine/aports/{package}/APKBUILD", f"pkgname={package}\n".encode()
            )
        alpine_source.write(
            "alpine/aports/fplinux-font-terminus/APKBUILD", b"pkgname=fplinux-font-terminus\n"
        )

        with mock.patch.object(alpine_registration, "COMMON_PACKAGES", common):
            selected = alpine_selection.selected_packages(
                {"rootfs": {"packages": list(platform)}},
                {
                    "rootfs": {
                        "base_packages": ["fplinux-font-terminus-6x12"],
                        "packages": [],
                        "exclude_packages": [
                            "fplinux-input",
                            "fplinux-usb-gadget",
                            "fplinux-ssh",
                        ],
                    }
                },
                alpine_source.root,
            )

        assert (selected) == (("fplinux-base", "fplinux-font-terminus-6x12", "fplinux-terminal"))

    def test_profile_rootfs_rejects_unknown_excludes_and_duplicate_additions(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A profile cannot silently remove or repeat an unowned rootfs package."""
        with mock.patch.object(
            alpine_registration, "COMMON_PACKAGES", (alpine_source.packages[0],)
        ):
            with pytest.raises(SystemExit, match="excludes a package not owned"):
                alpine_selection.selected_packages(
                    {"rootfs": {"packages": [alpine_source.packages[1]]}},
                    {
                        "rootfs": {
                            "base_packages": [],
                            "packages": [],
                            "exclude_packages": ["fplinux-missing"],
                        }
                    },
                    alpine_source.root,
                )
            with pytest.raises(SystemExit, match="duplicate base ownership"):
                alpine_selection.selected_packages(
                    {"rootfs": {"packages": [alpine_source.packages[1]]}},
                    {
                        "rootfs": {
                            "base_packages": [],
                            "packages": [alpine_source.packages[1]],
                            "exclude_packages": [],
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
            pytest.param("fplinux-terminal", id="terminal"),
            pytest.param("fplinux-brightness-ui", id="brightness-ui"),
            pytest.param("fplinux-showcase", id="showcase"),
        ],
    )
    @pytest.mark.parametrize(
        ("base", "profile", "excluded", "selected_font"),
        [
            pytest.param((), (), (), None, id="missing-font"),
            pytest.param(
                ("fplinux-font-terminus-6x12", "fplinux-font-terminus-8x16"),
                (),
                (),
                None,
                id="conflicting-base-fonts",
            ),
            pytest.param(
                ("fplinux-font-terminus-6x12",),
                ("fplinux-font-terminus-8x16",),
                (),
                None,
                id="conflicting-profile-font",
            ),
            pytest.param(
                ("fplinux-font-terminus-6x12",),
                (),
                ("fplinux-font-terminus-6x12",),
                None,
                id="excluded-font",
            ),
            pytest.param(
                ("fplinux-font-terminus-6x12",),
                (),
                (),
                "fplinux-font-terminus-6x12",
                id="small-font",
            ),
            pytest.param(
                ("fplinux-font-terminus-8x16",),
                (),
                (),
                "fplinux-font-terminus-8x16",
                id="large-font",
            ),
        ],
    )
    def test_text_rootfs_requires_one_font_payload(  # noqa: PLR0913 -- each parameter is an independent policy input.
        self,
        alpine_source: AlpineSourceContext,
        *,
        consumer: str,
        base: tuple[str, ...],
        profile: tuple[str, ...],
        excluded: tuple[str, ...],
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
                "exclude_packages": list(excluded),
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
