# SPDX-License-Identifier: GPL-2.0-only
"""Alpine selection scenarios."""

from __future__ import annotations

import unittest
from unittest import mock

from fplinux_cli.alpine import (
    lock as alpine_lock,
)
from fplinux_cli.alpine import (
    registration as alpine_registration,
)
from fplinux_cli.alpine import (
    selection as alpine_selection,
)

from tests.small.alpine import fixtures


class AlpineSelectionTests(fixtures.AlpineSourceFixture):
    """Protect Alpine selection behavior with controlled temporary inputs."""

    def test_selection_combines_common_and_platform_ownership(self) -> None:
        """Common and platform ownership contribute one canonical rootfs set."""
        third = "fplinux-package-c"
        self._write(f"alpine/aports/{third}/APKBUILD", f"pkgname={third}\n".encode())
        with mock.patch.object(alpine_registration, "COMMON_PACKAGES", (self.packages[0],)):
            selected = alpine_selection.selected_packages(
                {"rootfs": {"packages": [self.packages[1], third]}},
                {"rootfs": {"base_packages": [], "packages": [], "exclude_packages": []}},
                self.root,
            )
        self.assertEqual(selected, (*self.packages, third))

    def test_build_order_prepares_transitive_libraries_before_consumers(self) -> None:
        """A library that consumes another local library sees it in the sysroot."""
        dependencies = {
            "fplinux-app": ("fplinux-a-library",),
            "fplinux-a-library": ("fplinux-z-library",),
        }
        with mock.patch.object(alpine_registration, "LOCAL_BUILD_DEPENDENCIES", dependencies):
            order = alpine_selection.aport_build_order(("fplinux-app", "fplinux-a-library"))
            libraries = alpine_selection.local_build_dependencies(("fplinux-app",))
        self.assertEqual(set(order), {"fplinux-app", "fplinux-a-library", "fplinux-z-library"})
        self.assertEqual(len(order), 3)
        self.assertLess(order.index("fplinux-z-library"), order.index("fplinux-a-library"))
        self.assertLess(order.index("fplinux-a-library"), order.index("fplinux-app"))
        self.assertEqual(set(libraries), {"fplinux-a-library", "fplinux-z-library"})

    def test_selection_composes_target_base_with_profile_delta(self) -> None:
        """A normalized target base and profile delta retain only unexcluded packages."""
        target_package = "fplinux-package-c"
        profile_package = "fplinux-package-d"
        self._write(
            f"alpine/aports/{target_package}/APKBUILD", f"pkgname={target_package}\n".encode()
        )
        self._write(
            f"alpine/aports/{profile_package}/APKBUILD", f"pkgname={profile_package}\n".encode()
        )
        with mock.patch.object(alpine_registration, "COMMON_PACKAGES", (self.packages[0],)):
            selected = alpine_selection.selected_packages(
                {"rootfs": {"packages": [self.packages[1]]}},
                {
                    "rootfs": {
                        "base_packages": [target_package],
                        "packages": [profile_package],
                        "exclude_packages": [target_package],
                    }
                },
                self.root,
            )
        self.assertEqual(selected, (*self.packages, profile_package))

    def test_runtime_addition_is_selected_only_with_its_local_package(self) -> None:
        """A profile-only Alpine closure is unrelated to ordinary rootfs builds."""
        lock = {
            "runtime": {
                "packages": ["base-1-r0.apk"],
                "additions": {"fplinux-feature": ["feature-dependency-1-r0.apk"]},
            }
        }

        self.assertEqual(alpine_lock.runtime_package_names(lock, ()), ("base-1-r0.apk",))
        self.assertEqual(
            alpine_lock.runtime_package_names(lock, ("fplinux-feature",)),
            ("base-1-r0.apk", "feature-dependency-1-r0.apk"),
        )

    def test_bundle_subpackages_use_existing_aports_without_selecting_their_parents(self) -> None:
        """Optional child APK names remain distinct from the preinstalled producer packages."""
        for producer in ("fplinux-alsa-lib", "fplinux-bash", "fplinux-ncurses"):
            self._write(f"alpine/aports/{producer}/APKBUILD", f"pkgname={producer}\n".encode())
        optional = (
            "fplinux-alsa-lib-card-profiles",
            "fplinux-bash-loadables",
            "fplinux-ncurses-curses",
        )

        actual = alpine_selection.bundle_packages(
            {"bundle": {"packages": list(optional)}},
            {"bundle": {"packages": []}},
            ("fplinux-alsa-lib", "fplinux-bash", "fplinux-ncurses"),
            self.root,
        )

        self.assertEqual(actual, optional)

    def test_package_cannot_be_selected_and_bundle_published(self) -> None:
        """One package cannot be both installed and published separately."""
        with self.assertRaisesRegex(SystemExit, "both rootfs-selected and bundle-published"):
            alpine_selection.bundle_packages(
                {"bundle": {"packages": [self.packages[0]]}},
                {"bundle": {"packages": []}},
                self.packages,
                self.root,
            )

    def test_profile_preinstall_removes_only_its_platform_optional_package(self) -> None:
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

        self.assertEqual(
            alpine_selection.bundle_packages(platform, profile, ("fplinux-package-a",), self.root),
            ("fplinux-package-b",),
        )
        with self.assertRaisesRegex(SystemExit, "both rootfs-selected and bundle-published"):
            alpine_selection.bundle_packages(
                {"bundle": {"packages": []}},
                {**profile, "bundle": {"packages": ["fplinux-package-a"]}},
                ("fplinux-package-a",),
                self.root,
            )
        with self.assertRaisesRegex(SystemExit, "owned by both platform and target"):
            alpine_selection.bundle_packages(
                platform,
                {**profile, "bundle": {"packages": ["fplinux-package-a"]}},
                ("fplinux-package-a",),
                self.root,
            )

    def test_bundle_selection_rejects_duplicate_platform_and_target_ownership(self) -> None:
        """A bundle package has one declarative owner, just like a rootfs package."""
        with self.assertRaisesRegex(SystemExit, "owned by both platform and target"):
            alpine_selection.bundle_packages(
                {"bundle": {"packages": [self.packages[0]]}},
                {"bundle": {"packages": [self.packages[0]]}},
                (),
                self.root,
            )

    def test_selection_rejects_duplicate_ownership(self) -> None:
        """One package cannot be owned by both common and platform layers."""
        with (
            mock.patch.object(alpine_registration, "COMMON_PACKAGES", (self.packages[0],)),
            self.assertRaisesRegex(SystemExit, "owned by both common and platform"),
        ):
            alpine_selection.selected_packages(
                {"rootfs": {"packages": [self.packages[0]]}},
                {"rootfs": {"base_packages": [], "packages": [], "exclude_packages": []}},
                self.root,
            )

    def test_selected_profile_can_replace_platform_rootfs_packages(self) -> None:
        """A profile delta removes declared base packages and adds a distinct package."""
        extra = "fplinux-package-c"
        self._write(f"alpine/aports/{extra}/APKBUILD", f"pkgname={extra}\n".encode())
        with mock.patch.object(alpine_registration, "COMMON_PACKAGES", (self.packages[0],)):
            selected = alpine_selection.selected_packages(
                {"rootfs": {"packages": [self.packages[1]]}},
                {
                    "rootfs": {
                        "base_packages": [],
                        "packages": [extra],
                        "exclude_packages": [self.packages[1]],
                    }
                },
                self.root,
            )
        self.assertEqual(selected, (self.packages[0], extra))

    def test_profile_can_exclude_gadget_input_stack_and_retain_terminal(self) -> None:
        """A host-only rootfs removes gadget-only input without losing the terminal."""
        common = ("fplinux-base", "fplinux-terminal", "fplinux-input")
        platform = ("fplinux-usb-gadget", "fplinux-ssh")
        for package in (*common, *platform):
            self._write(f"alpine/aports/{package}/APKBUILD", f"pkgname={package}\n".encode())
        self._write(
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
                self.root,
            )

        self.assertEqual(
            selected, ("fplinux-base", "fplinux-font-terminus-6x12", "fplinux-terminal")
        )

    def test_profile_rootfs_rejects_unknown_excludes_and_duplicate_additions(self) -> None:
        """A profile cannot silently remove or repeat an unowned rootfs package."""
        with mock.patch.object(alpine_registration, "COMMON_PACKAGES", (self.packages[0],)):
            with self.assertRaisesRegex(SystemExit, "excludes a package not owned"):
                alpine_selection.selected_packages(
                    {"rootfs": {"packages": [self.packages[1]]}},
                    {
                        "rootfs": {
                            "base_packages": [],
                            "packages": [],
                            "exclude_packages": ["fplinux-missing"],
                        }
                    },
                    self.root,
                )
            with self.assertRaisesRegex(SystemExit, "duplicate base ownership"):
                alpine_selection.selected_packages(
                    {"rootfs": {"packages": [self.packages[1]]}},
                    {
                        "rootfs": {
                            "base_packages": [],
                            "packages": [self.packages[1]],
                            "exclude_packages": [],
                        }
                    },
                    self.root,
                )

    def test_lock_requires_every_selected_artifact_to_be_declared(self) -> None:
        """Reject a runtime/sysroot package without its exact artifact record."""
        lock = alpine_lock.load_alpine_lock(self.root)
        self.assertEqual(lock["arch"], "armv7")
        path = self.root / "alpine.lock.toml"
        text = path.read_text().replace(
            'packages = ["musl-dev-1-r0.apk"]',
            'packages = ["missing.apk"]',
        )
        path.write_text(text)
        with self.assertRaisesRegex(SystemExit, "has no locked artifact"):
            alpine_lock.load_alpine_lock(self.root)

    def test_lock_rejects_an_unknown_field(self) -> None:
        """The exact lock shape rejects unrecognized metadata."""
        path = self.root / "alpine.lock.toml"
        path.write_text('unexpected = "value"\n' + path.read_text())
        with self.assertRaisesRegex(SystemExit, "invalid Alpine lock"):
            alpine_lock.load_alpine_lock(self.root)

    def test_text_rootfs_requires_one_font_payload(self) -> None:
        """Text applications cannot start with no default font or competing defaults."""
        self._write(
            "alpine/aports/fplinux-font-terminus/APKBUILD", b"pkgname=fplinux-font-terminus\n"
        )
        consumers = ("fplinux-terminal", "fplinux-brightness-ui", "fplinux-showcase")
        for name in consumers:
            self._write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
        small = "fplinux-font-terminus-6x12"
        large = "fplinux-font-terminus-8x16"
        for consumer in consumers:
            for base, profile, excluded in (
                ([], [], []),
                ([small, large], [], []),
                ([small], [large], []),
                ([small], [], [small]),
            ):
                with (
                    self.subTest(consumer=consumer, base=base, profile=profile, excluded=excluded),
                    mock.patch.object(alpine_registration, "COMMON_PACKAGES", (consumer,)),
                    self.assertRaisesRegex(SystemExit, "exactly one Terminus font size"),
                ):
                    alpine_selection.selected_packages(
                        {"rootfs": {"packages": []}},
                        {
                            "rootfs": {
                                "base_packages": base,
                                "packages": profile,
                                "exclude_packages": excluded,
                            }
                        },
                        self.root,
                    )
            for selected_font in (small, large):
                with (
                    self.subTest(consumer=consumer, font=selected_font),
                    mock.patch.object(alpine_registration, "COMMON_PACKAGES", (consumer,)),
                ):
                    selected = alpine_selection.selected_packages(
                        {"rootfs": {"packages": []}},
                        {
                            "rootfs": {
                                "base_packages": [selected_font],
                                "packages": [],
                                "exclude_packages": [],
                            }
                        },
                        self.root,
                    )
                    self.assertEqual(set(selected), {consumer, selected_font})


if __name__ == "__main__":
    unittest.main()
