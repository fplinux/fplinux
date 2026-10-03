# SPDX-License-Identifier: GPL-2.0-only
"""Focused tests for the one content-addressed Alpine rootfs state."""

from __future__ import annotations

import hashlib
import json
import os
import py_compile
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

from fplinux_cli import alpine_builder, alpine_state

if TYPE_CHECKING:
    from collections.abc import Callable


class AlpineStateTests(unittest.TestCase):
    """Keep selected Alpine inputs, outputs and receipts exact."""

    def setUp(self) -> None:
        """Create one complete minimal Alpine rootfs recipe fixture."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        self.signing_key = "d" * 64
        self.packages = ("fplinux-package-a", "fplinux-package-b")
        self._write(
            "alpine.lock.toml",
            b'release = "3.24.1"\n'
            b'branch = "v3.24"\n'
            b'arch = "armv7"\n'
            b'triplet = "armv7-alpine-linux-musleabihf"\n'
            b"\n[repositories]\n"
            b'main = "https://example.invalid/alpine/v3.24/main"\n'
            b'community = "https://example.invalid/alpine/v3.24/community"\n'
            b"\n[minirootfs]\n"
            b'url = "https://example.invalid/alpine-minirootfs.tar.gz"\n'
            b'sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"\n'
            b"bytes = 1\n"
            b"\n[runtime]\n"
            b'packages = ["openrc-1-r0.apk"]\n'
            b"\n[runtime.additions]\n"
            b"\n[sysroot]\n"
            b'packages = ["musl-dev-1-r0.apk"]\n'
            b"\n[[package]]\n"
            b'repository = "main"\n'
            b'file = "openrc-1-r0.apk"\n'
            b'sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"\n'
            b"bytes = 2\n"
            b"\n[[package]]\n"
            b'repository = "main"\n'
            b'file = "musl-dev-1-r0.apk"\n'
            b'sha256 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"\n'
            b"bytes = 3\n",
        )
        self._write("alpine/abuild.conf", b"PACKAGER=FPLinux\n")
        for name in self.packages:
            self._write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
        self.aport = self.root / "alpine/aports/fplinux-package-a/APKBUILD"
        self._write("alpine/aports/not-production/APKBUILD", b"pkgname=not-production\n")
        self._write("scripts/fplinux_cli/alpine_state.py", b"state implementation\n")
        self._write("scripts/fplinux_cli/build/kernel.py", b"builder implementation\n")
        self._write("scripts/fplinux_cli/alpine_builder.py", b"Alpine builder implementation\n")
        self._write("scripts/fplinux_cli/common.py", b"shared archive and file operations\n")
        self._write("scripts/fplinux_cli/build_env.py", b"build environment\n")
        self._write("scripts/fplinux_cli/firmware_inputs.py", b"firmware inputs\n")
        self.shared_source = self._write("alpine/shared/shared.c", b"int shared;\n")
        self.bootstrap = self._write("common/ramroot-init.sh", b"#!/bin/sh\nexit 0\n")

    def _write(self, relative: str, contents: bytes) -> Path:
        """Write one fixture file below the temporary source root."""
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def _recipe(
        self,
        image: str = "1" * 64,
        signing_key: str | None = None,
        packages: tuple[str, ...] | None = None,
        display_brightness: dict[str, Any] | None = None,
        *,
        root_kind: str = "initramfs",
    ) -> str:
        return alpine_state.alpine_rootfs_recipe(
            image,
            self.signing_key if signing_key is None else signing_key,
            self.packages if packages is None else packages,
            self.root,
            display_brightness=display_brightness,
            root_kind=root_kind,
        )

    def test_selection_combines_common_and_platform_ownership(self) -> None:
        """Common and platform ownership contribute one canonical rootfs set."""
        third = "fplinux-package-c"
        self._write(f"alpine/aports/{third}/APKBUILD", f"pkgname={third}\n".encode())
        with mock.patch.object(alpine_state, "COMMON_PACKAGES", (self.packages[0],)):
            selected = alpine_state.selected_packages(
                {"rootfs": {"packages": [self.packages[1], third]}},
                {"rootfs": {"base_packages": [], "packages": [], "exclude_packages": []}},
                self.root,
            )
        self.assertEqual(selected, (*self.packages, third))

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
        with mock.patch.object(alpine_state, "COMMON_PACKAGES", (self.packages[0],)):
            selected = alpine_state.selected_packages(
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

        self.assertEqual(alpine_state.runtime_package_names(lock, ()), ("base-1-r0.apk",))
        self.assertEqual(
            alpine_state.runtime_package_names(lock, ("fplinux-feature",)),
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

        actual = alpine_state.bundle_packages(
            {"bundle": {"packages": list(optional)}},
            {"bundle": {"packages": []}},
            ("fplinux-alsa-lib", "fplinux-bash", "fplinux-ncurses"),
            self.root,
        )

        self.assertEqual(actual, optional)

    def test_subpackage_rootfs_receipt_tracks_output_selection_and_producer_changes(self) -> None:
        """The chosen child is causal, producer edits miss, and unrelated edits still hit."""
        producer = "fplinux-ncurses"
        child = "fplinux-ncurses-curses"
        aport = self._write(f"alpine/aports/{producer}/APKBUILD", b"pkgname=fplinux-ncurses\n")
        output = self.root / "built-rootfs"
        output.mkdir()
        (output / "rootfs.cpio").write_bytes(b"rootfs content\n")
        recipe = self._recipe(packages=(child,))
        alpine_state.write_receipt(output, recipe)

        self.assertTrue(alpine_state.receipt_matches(output, self._recipe(packages=(child,))))
        self.assertNotEqual(recipe, self._recipe(packages=(producer,)))
        self.assertEqual(
            alpine_state.alpine_package_recipe(child, "1" * 64, self.signing_key, self.root),
            alpine_state.alpine_package_recipe(producer, "1" * 64, self.signing_key, self.root),
        )
        self._write("alpine/aports/not-production/APKBUILD", b"unrelated changed\n")
        self.assertTrue(alpine_state.receipt_matches(output, self._recipe(packages=(child,))))
        aport.write_bytes(b"pkgname=fplinux-ncurses\nchanged=yes\n")
        self.assertFalse(alpine_state.receipt_matches(output, self._recipe(packages=(child,))))

    def test_materializing_a_subpackage_copies_the_producer_sources(self) -> None:
        """A child name stages its existing producer instead of requiring a new source tree."""
        self._write("alpine/aports/fplinux-ncurses/APKBUILD", b"pkgname=fplinux-ncurses\n")
        self._write("alpine/aports/fplinux-ncurses/adapter.c", b"producer adapter\n")
        destination = self.root / "stage"

        alpine_builder.materialize_aport_sources("fplinux-ncurses-curses", self.root, destination)

        self.assertEqual((destination / "APKBUILD").read_bytes(), b"pkgname=fplinux-ncurses\n")
        self.assertEqual((destination / "adapter.c").read_bytes(), b"producer adapter\n")

    def test_package_cannot_be_selected_and_bundle_published(self) -> None:
        """One package cannot be both installed and published separately."""
        with self.assertRaisesRegex(SystemExit, "both rootfs-selected and bundle-published"):
            alpine_state.bundle_packages(
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
            alpine_state.bundle_packages(platform, profile, ("fplinux-package-a",), self.root),
            ("fplinux-package-b",),
        )
        with self.assertRaisesRegex(SystemExit, "both rootfs-selected and bundle-published"):
            alpine_state.bundle_packages(
                {"bundle": {"packages": []}},
                {**profile, "bundle": {"packages": ["fplinux-package-a"]}},
                ("fplinux-package-a",),
                self.root,
            )
        with self.assertRaisesRegex(SystemExit, "owned by both platform and target"):
            alpine_state.bundle_packages(
                platform,
                {**profile, "bundle": {"packages": ["fplinux-package-a"]}},
                ("fplinux-package-a",),
                self.root,
            )

    def test_bundle_selection_rejects_duplicate_platform_and_target_ownership(self) -> None:
        """A bundle package has one declarative owner, just like a rootfs package."""
        with self.assertRaisesRegex(SystemExit, "owned by both platform and target"):
            alpine_state.bundle_packages(
                {"bundle": {"packages": [self.packages[0]]}},
                {"bundle": {"packages": [self.packages[0]]}},
                (),
                self.root,
            )

    def test_selection_rejects_duplicate_ownership(self) -> None:
        """One package cannot be owned by both common and platform layers."""
        with (
            mock.patch.object(alpine_state, "COMMON_PACKAGES", (self.packages[0],)),
            self.assertRaisesRegex(SystemExit, "owned by both common and platform"),
        ):
            alpine_state.selected_packages(
                {"rootfs": {"packages": [self.packages[0]]}},
                {"rootfs": {"base_packages": [], "packages": [], "exclude_packages": []}},
                self.root,
            )

    def test_selected_profile_can_replace_platform_rootfs_packages(self) -> None:
        """A profile delta removes declared base packages and adds a distinct package."""
        extra = "fplinux-package-c"
        self._write(f"alpine/aports/{extra}/APKBUILD", f"pkgname={extra}\n".encode())
        with mock.patch.object(alpine_state, "COMMON_PACKAGES", (self.packages[0],)):
            selected = alpine_state.selected_packages(
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

        with mock.patch.object(alpine_state, "COMMON_PACKAGES", common):
            selected = alpine_state.selected_packages(
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
        with mock.patch.object(alpine_state, "COMMON_PACKAGES", (self.packages[0],)):
            with self.assertRaisesRegex(SystemExit, "excludes a package not owned"):
                alpine_state.selected_packages(
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
                alpine_state.selected_packages(
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
        lock = alpine_state.load_alpine_lock(self.root)
        self.assertEqual(lock["arch"], "armv7")
        path = self.root / "alpine.lock.toml"
        text = path.read_text().replace(
            'packages = ["musl-dev-1-r0.apk"]',
            'packages = ["missing.apk"]',
        )
        path.write_text(text)
        with self.assertRaisesRegex(SystemExit, "has no locked artifact"):
            alpine_state.load_alpine_lock(self.root)

    def test_lock_rejects_an_unknown_field(self) -> None:
        """The exact lock shape rejects unrecognized metadata."""
        path = self.root / "alpine.lock.toml"
        path.write_text('unexpected = "value"\n' + path.read_text())
        with self.assertRaisesRegex(SystemExit, "invalid Alpine lock"):
            alpine_state.load_alpine_lock(self.root)

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
        package_before = alpine_state.alpine_package_recipe(
            self.packages[0], "1" * 64, self.signing_key, self.root
        )
        self._write("scripts/fplinux_cli/build/kernel.py", b"kernel implementation changed\n")
        self.assertEqual(rootfs_before, self._recipe())
        self.assertEqual(
            package_before,
            alpine_state.alpine_package_recipe(
                self.packages[0], "1" * 64, self.signing_key, self.root
            ),
        )

    def test_aport_python_bytecode_preserves_receipt_and_package_recipe(self) -> None:
        """Compiling a helper is unrelated; changing its source invalidates its artifacts."""
        helper = self._write("alpine/aports/fplinux-package-a/helper.py", b"VALUE = 42\n")
        rootfs_recipe = self._recipe()
        package_recipe = alpine_state.alpine_package_recipe(
            self.packages[0], "1" * 64, self.signing_key, self.root
        )
        output = self.root / "cached-rootfs"
        output.mkdir()
        (output / "rootfs.cpio").write_bytes(b"logical composition\n")
        alpine_state.write_receipt(output, rootfs_recipe)
        self.assertTrue(alpine_state.receipt_matches(output, self._recipe()))

        py_compile.compile(str(helper), doraise=True)
        self.assertTrue(alpine_state.receipt_matches(output, self._recipe()))
        self.assertEqual(
            package_recipe,
            alpine_state.alpine_package_recipe(
                self.packages[0], "1" * 64, self.signing_key, self.root
            ),
        )

        helper.write_bytes(b"VALUE = 43\n")
        self.assertFalse(alpine_state.receipt_matches(output, self._recipe()))
        self.assertNotEqual(
            package_recipe,
            alpine_state.alpine_package_recipe(
                self.packages[0], "1" * 64, self.signing_key, self.root
            ),
        )

    def test_alpine_preparation_changes_invalidate_rootfs_and_package_recipes(self) -> None:
        """Build and shared extraction changes invalidate the artifacts they prepare."""
        for source in ("alpine_builder.py", "common.py"):
            with self.subTest(source=source):
                rootfs_before = self._recipe()
                package_before = alpine_state.alpine_package_recipe(
                    self.packages[0], "1" * 64, self.signing_key, self.root
                )
                self._write(f"scripts/fplinux_cli/{source}", b"preparation changed\n")
                self.assertNotEqual(rootfs_before, self._recipe())
                self.assertNotEqual(
                    package_before,
                    alpine_state.alpine_package_recipe(
                        self.packages[0], "1" * 64, self.signing_key, self.root
                    ),
                )

    def test_build_environment_bytes_change_alpine_recipes(self) -> None:
        """A shared deterministic-environment edit invalidates rootfs and APK slots."""
        rootfs_before = self._recipe()
        package_before = alpine_state.alpine_package_recipe(
            self.packages[0], "1" * 64, self.signing_key, self.root
        )

        self._write("scripts/fplinux_cli/build_env.py", b"changed environment\n")

        self.assertNotEqual(rootfs_before, self._recipe())
        self.assertNotEqual(
            package_before,
            alpine_state.alpine_package_recipe(
                self.packages[0], "1" * 64, self.signing_key, self.root
            ),
        )

    def test_aport_edit_invalidates_only_its_package_recipe(self) -> None:
        """An ordinary local edit does not rebuild unrelated FPLinux APKs."""
        before = {
            name: alpine_state.alpine_package_recipe(
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
            name: alpine_state.alpine_package_recipe(
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
        with mock.patch.object(alpine_state, "SHARED_APORT_SOURCES", mapping):
            before = {
                name: alpine_state.alpine_package_recipe(
                    name, "1" * 64, self.signing_key, self.root
                )
                for name in self.packages
            }
            rootfs_before = self._recipe()
            self.shared_source.write_bytes(b"int shared = 1;\n")
            after = {
                name: alpine_state.alpine_package_recipe(
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
        repository = Path(__file__).resolve().parents[2]
        for directory in ("lib/fplinux", "include/fplinux"):
            shutil.copytree(repository / directory, self.root / directory)
        font_source = self._write("lib/fplinux/fplinux-font.c", b"fixture original font reader\n")
        self._write("include/fplinux/fplinux-font.h", b"fixture font interface\n")
        before = {
            name: alpine_state.alpine_package_recipe(name, "1" * 64, self.signing_key, self.root)
            for name in (*consumers, unrelated)
        }

        font_source.write_bytes(b"fixture changed font reader\n")

        for name in consumers:
            with self.subTest(consumer=name):
                after = alpine_state.alpine_package_recipe(
                    name, "1" * 64, self.signing_key, self.root
                )
                self.assertNotEqual(before[name], after)
                stage = self.root / "stages" / name
                alpine_builder.materialize_aport_sources(name, self.root, stage)
                self.assertEqual(
                    (stage / "fplinux-font.c").read_bytes(), b"fixture changed font reader\n"
                )
                self.assertEqual(
                    (stage / "fplinux-font.h").read_bytes(), b"fixture font interface\n"
                )
        self.assertEqual(
            before[unrelated],
            alpine_state.alpine_package_recipe(unrelated, "1" * 64, self.signing_key, self.root),
        )

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
                    mock.patch.object(alpine_state, "COMMON_PACKAGES", (consumer,)),
                    self.assertRaisesRegex(SystemExit, "exactly one Terminus font size"),
                ):
                    alpine_state.selected_packages(
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
                    mock.patch.object(alpine_state, "COMMON_PACKAGES", (consumer,)),
                ):
                    selected = alpine_state.selected_packages(
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
            for source in alpine_state.shared_aport_sources(name, self.root):
                self._write(source.relative_to(self.root).as_posix(), b"shared source\n")

        cases = (
            ("fplinux-libdrm", "library.c", {"fplinux-present", "fplinux-terminal"}),
            ("fplinux-libtsm", "APKBUILD", {"fplinux-terminal"}),
            ("fplinux-libxkbcommon", "APKBUILD", {"fplinux-terminal"}),
        )
        for library, filename, consumers in cases:
            with self.subTest(library=library):
                before = {
                    name: alpine_state.alpine_package_recipe(
                        name, "1" * 64, self.signing_key, self.root
                    )
                    for name in packages
                }
                rootfs_before = self._recipe(packages=("fplinux-terminal",))
                self._write(f"alpine/aports/{library}/{filename}", b"changed library input\n")
                for name in packages:
                    after = alpine_state.alpine_package_recipe(
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

    def test_apk_cache_control_flow_with_mocked_abuild_preserves_last_good(self) -> None:
        """Exercise cache reuse, one-source invalidation and failure using fake abuild output."""
        cache = Path(self.temporary.name) / "cache"
        sources = cache / "downloads/alpine/sources"
        sources.mkdir(parents=True)
        private_key = self._write("keys/fplinux-build.rsa", b"private key\n")
        public_key = self._write("keys/fplinux-build.rsa.pub", b"public key\n")
        builds: list[str] = []
        failing_package: str | None = None
        changed_package = self.packages[1]
        child_package = "fplinux-package-a-extra"
        original_recipe = alpine_state.alpine_package_recipe

        def package_recipe(name: str, image: str, signing_key: str) -> str:
            return original_recipe(name, image, signing_key, root=self.root)

        def list_packages(command: list[str], cwd: Path, environment: dict[str, str]) -> str:
            del command, environment
            names = (cwd.name, child_package) if cwd.name == self.packages[0] else (cwd.name,)
            return "".join(f"{name}-1.0-r0.apk\n" for name in names)

        def run_as_builder(command: list[str], cwd: Path, environment: dict[str, str]) -> None:
            nonlocal failing_package
            if command == ["apkbuild-lint", "APKBUILD"]:
                return
            if cwd.name == failing_package:
                raise SystemExit(f"build failed: abuild failed for {cwd.name}")
            builds.append(cwd.name)
            output = Path(environment["REPODEST"])
            output.mkdir(parents=True, exist_ok=True)
            names = (cwd.name, child_package) if cwd.name == self.packages[0] else (cwd.name,)
            for name in names:
                (output / f"{name}-1.0-r0.apk").write_text(f"{name}\n", encoding="utf-8")

        invocation = 0

        def build_apks() -> tuple[dict[str, Path], Path, Path]:
            nonlocal invocation
            work = Path(self.temporary.name) / f"work-{invocation}"
            invocation += 1
            work.mkdir()
            return alpine_builder._build_fplinux_apks(  # noqa: SLF001
                {"triplet": "armv7-alpine-linux-musleabihf"},
                sysroot=Path(self.temporary.name) / "sysroot",
                work=work,
                jobs=1,
                private_key=private_key,
                public_key=public_key,
                build_packages=(*self.packages, child_package),
            )

        with (
            mock.patch.object(alpine_builder, "CACHE", cache),
            mock.patch.object(alpine_builder, "ROOT", self.root),
            mock.patch.object(os, "geteuid", return_value=0),
            mock.patch.dict(os.environ, {"FPLINUX_CONTAINER_IMAGE_RECIPE": "1" * 64}),
            mock.patch.object(alpine_builder, "_alpine_source_cache", return_value=sources),
            mock.patch.object(alpine_builder, "_chown_tree"),
            mock.patch.object(alpine_builder, "_builder_output", side_effect=list_packages),
            mock.patch.object(alpine_builder, "_run_as_builder", side_effect=run_as_builder),
            mock.patch.object(
                alpine_builder,
                "_apk_package_name",
                side_effect=lambda path: path.name.removesuffix("-1.0-r0.apk"),
            ),
            mock.patch.object(alpine_builder, "_log_message"),
            mock.patch.object(alpine_state, "alpine_package_recipe", side_effect=package_recipe),
            mock.patch.object(
                alpine_state, "SUBPACKAGE_APORTS", {child_package: self.packages[0]}
            ),
        ):
            outputs, _public, _private = build_apks()
            self.assertEqual(builds, list(self.packages))
            self.assertEqual(outputs[child_package].read_bytes(), b"fplinux-package-a-extra\n")

            builds.clear()
            outputs, _public, _private = build_apks()
            self.assertEqual(builds, [])
            self.assertEqual(outputs[child_package].read_bytes(), b"fplinux-package-a-extra\n")

            self._write("alpine/aports/not-production/APKBUILD", b"unrelated changed\n")
            build_apks()
            self.assertEqual(builds, [])

            self._write(
                f"alpine/aports/{changed_package}/APKBUILD",
                f"pkgname={changed_package}\npkgrel=1\n".encode(),
            )
            build_apks()
            self.assertEqual(builds, [changed_package])

            slot = cache / alpine_state.PACKAGE_CACHE_DIRECTORY / changed_package
            receipt = slot / alpine_state.PACKAGE_RECEIPT_NAME
            package = slot / f"{changed_package}-1.0-r0.apk"
            previous_receipt = receipt.read_bytes()
            previous_package = package.read_bytes()
            self._write(
                f"alpine/aports/{changed_package}/APKBUILD",
                f"pkgname={changed_package}\npkgrel=2\n".encode(),
            )
            failing_package = changed_package
            with self.assertRaisesRegex(SystemExit, f"abuild failed for {changed_package}"):
                build_apks()

        self.assertEqual(receipt.read_bytes(), previous_receipt)
        self.assertEqual(package.read_bytes(), previous_package)

    def test_local_library_apks_populate_fresh_sysroots_on_builds_and_cache_hits(self) -> None:
        """Fake abuild/APK processes exercise dependency files and causal cache decisions."""
        consumer = "fplinux-present"
        library = "fplinux-libdrm"
        unrelated = self.packages[0]
        for name in (consumer, library):
            self._write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
            for source in alpine_state.shared_aport_sources(name, self.root):
                self._write(source.relative_to(self.root).as_posix(), b"shared source\n")
        library_source = self._write(f"alpine/aports/{library}/library.c", b"library version 1\n")
        cache = Path(self.temporary.name) / "cache"
        sources = cache / "downloads/alpine/sources"
        sources.mkdir(parents=True)
        private_key = self._write("keys/fplinux-build.rsa", b"private key\n")
        public_key = self._write("keys/fplinux-build.rsa.pub", b"public key\n")
        builds: list[str] = []
        original_recipe = alpine_state.alpine_package_recipe

        def package_recipe(name: str, image: str, signing_key: str) -> str:
            return original_recipe(name, image, signing_key, root=self.root)

        def list_packages(command: list[str], cwd: Path, environment: dict[str, str]) -> str:
            del command, environment
            names = (library, f"{library}-dev") if cwd.name == library else (cwd.name,)
            return "\n".join(f"{name}-1.0-r0.apk" for name in names)

        def run_as_builder(command: list[str], cwd: Path, environment: dict[str, str]) -> None:
            if command == ["apkbuild-lint", "APKBUILD"]:
                return
            sysroot = Path(environment["CBUILDROOT"])
            if cwd.name == consumer:
                for installed in (library, f"{library}-dev"):
                    self.assertEqual(
                        (sysroot / installed).read_bytes(), library_source.read_bytes()
                    )
            builds.append(cwd.name)
            repository = Path(environment["REPODEST"])
            for filename in list_packages(command, cwd, environment).splitlines():
                (repository / filename).write_bytes(library_source.read_bytes())

        def install_apks(command: list[str]) -> None:
            """Replace APK installation with file markers; signature checks remain untested."""
            self.assertEqual(command[0], "apk")
            self.assertIn("--no-network", command)
            self.assertIn("--no-scripts", command)
            self.assertNotIn("--allow-untrusted", command)
            keys = Path(command[command.index("--keys-dir") + 1])
            self.assertEqual((keys / public_key.name).read_bytes(), public_key.read_bytes())
            sysroot = Path(command[command.index("--root") + 1])
            for filename in command[command.index("add") + 1 :]:
                apk = Path(filename)
                name = apk.name.removesuffix("-1.0-r0.apk")
                (sysroot / name).write_bytes(apk.read_bytes())

        invocation = 0

        def build_apks() -> None:
            nonlocal invocation
            work = Path(self.temporary.name) / f"library-work-{invocation}"
            invocation += 1
            sysroot = work / "sysroot"
            sysroot.mkdir(parents=True)
            alpine_builder._build_fplinux_apks(  # noqa: SLF001
                {"arch": "armv7", "triplet": "armv7-alpine-linux-musleabihf"},
                sysroot=sysroot,
                work=work,
                jobs=1,
                private_key=private_key,
                public_key=public_key,
                build_packages=(consumer, unrelated),
            )
            for installed in (library, f"{library}-dev"):
                self.assertEqual((sysroot / installed).read_bytes(), library_source.read_bytes())

        with (
            mock.patch.object(alpine_builder, "CACHE", cache),
            mock.patch.object(alpine_builder, "ROOT", self.root),
            mock.patch.object(os, "geteuid", return_value=0),
            mock.patch.dict(os.environ, {"FPLINUX_CONTAINER_IMAGE_RECIPE": "1" * 64}),
            mock.patch.object(alpine_builder, "_alpine_source_cache", return_value=sources),
            mock.patch.object(alpine_builder, "_chown_tree"),
            mock.patch.object(alpine_builder, "_builder_output", side_effect=list_packages),
            mock.patch.object(alpine_builder, "_run_as_builder", side_effect=run_as_builder),
            mock.patch.object(alpine_builder, "_run", side_effect=install_apks),
            mock.patch.object(
                alpine_builder,
                "_apk_package_name",
                side_effect=lambda path: path.name.removesuffix("-1.0-r0.apk"),
            ),
            mock.patch.object(alpine_builder, "_log_message"),
            mock.patch.object(alpine_state, "alpine_package_recipe", side_effect=package_recipe),
        ):
            build_apks()
            self.assertEqual(set(builds), {library, consumer, unrelated})
            builds.clear()
            build_apks()
            self.assertEqual(builds, [])
            self._write(f"alpine/aports/{consumer}/program.c", b"changed consumer\n")
            build_apks()
            self.assertEqual(builds, [consumer])
            builds.clear()
            library_source.write_bytes(b"library version 2\n")
            build_apks()
            self.assertEqual(set(builds), {library, consumer})

    def test_cached_rootfs_reuse_honors_mocked_bundle_solver_results(self) -> None:
        """Stub APK builds and CPIO/solver processes; reuse must honor installation errors."""
        cache = Path(self.temporary.name) / "cache"
        output = cache / "rootfs" / ("9" * 64)
        output.mkdir(parents=True)
        rootfs = output / alpine_state.ROOTFS_NAME
        rootfs.write_bytes(b"rootfs\n")
        archive = self._write("downloads/minirootfs.tar.gz", b"archive\n")
        private_key = self._write("keys/fplinux-build.rsa", b"private\n")
        public_key = self._write("keys/fplinux-build.rsa.pub", b"public\n")
        rootfs_package, bundle_package = self.packages
        base_apk = self._write(f"built/{rootfs_package}.apk", b"base\n")
        bundle_apk = self._write(f"built/{bundle_package}.apk", b"bundle\n")
        extracted = mock.MagicMock()
        archive_context = mock.MagicMock()
        archive_context.__enter__.return_value = extracted
        lock = {
            "release": "3.24.1",
            "arch": "armv7",
            "minirootfs": {
                "url": "https://example.invalid",
                "sha256": "a" * 64,
                "bytes": archive.stat().st_size,
            },
        }

        for cached_bundle, solver_status in ((False, 0), (False, 7), (True, 0), (True, 7)):

            def run_external(
                command: list[str],
                *,
                error_status: int = solver_status,
                **_kwargs: object,
            ) -> subprocess.CompletedProcess[str]:
                status = error_status if "--simulate" in command else 0
                detail = "missing runtime dependency" if status else ""
                return subprocess.CompletedProcess(command, status, "", detail)

            cached_outputs = {bundle_package: bundle_apk} if cached_bundle else None
            with (
                self.subTest(cached_bundle=cached_bundle, solver_status=solver_status),
                mock.patch.object(alpine_builder, "CACHE", cache),
                mock.patch.dict(os.environ, {"FPLINUX_CONTAINER_IMAGE_RECIPE": "1" * 64}),
                mock.patch.object(
                    alpine_builder,
                    "_ensure_apk_signing_key",
                    return_value=(private_key, public_key, "2" * 64),
                ),
                mock.patch.object(alpine_state, "alpine_rootfs_recipe", return_value="9" * 64),
                mock.patch.object(alpine_state, "receipt_matches", return_value=True),
                mock.patch.object(
                    alpine_builder,
                    "_cached_aport_packages",
                    side_effect=({rootfs_package: base_apk}, cached_outputs),
                ),
                mock.patch.object(alpine_state, "load_alpine_lock", return_value=lock),
                mock.patch.object(alpine_state, "package_records", return_value={}),
                mock.patch.object(alpine_builder, "_fetch", return_value=archive),
                mock.patch.object(alpine_builder, "_alpine_runtime_packages", return_value=[]),
                mock.patch.object(alpine_builder, "_alpine_group_packages", return_value=[]),
                mock.patch.object(tarfile, "open", return_value=archive_context),
                mock.patch.object(alpine_builder, "_prepare_alpine_sysroot"),
                mock.patch.object(alpine_builder, "_log_message"),
                mock.patch.object(
                    alpine_builder,
                    "_build_fplinux_apks",
                    return_value=(
                        {rootfs_package: base_apk, bundle_package: bundle_apk},
                        private_key,
                        public_key,
                    ),
                ),
                mock.patch.object(
                    alpine_builder,
                    "_build_alpine_composition_repository",
                    side_effect=AssertionError("rootfs cache hit must not recompose the rootfs"),
                ),
                mock.patch.object(subprocess, "run", side_effect=run_external),
            ):
                if solver_status:
                    with self.assertRaisesRegex(SystemExit, "offline: missing runtime dependency"):
                        alpine_builder.build_rootfs(2, (rootfs_package,), (bundle_package,))
                else:
                    actual = alpine_builder.build_rootfs(2, (rootfs_package,), (bundle_package,))
                    self.assertEqual(actual[:3], (rootfs, output, "9" * 64))
                    self.assertEqual(actual[3], {bundle_package: bundle_apk})
                self.assertEqual(rootfs.read_bytes(), b"rootfs\n")

    def test_rootfs_cache_hits_reuse_cached_outputs(self) -> None:
        """Rootfs cache hits reuse cached outputs for distinct recipes."""
        cache = Path(self.temporary.name) / "cache"
        package = self.packages[0]
        recipes = ("3" * 64, "4" * 64)
        cached_package = self._write("cached/package.apk", b"package\n")
        for recipe in recipes:
            output = cache / "rootfs" / recipe
            output.mkdir(parents=True)
            rootfs = output / alpine_state.ROOTFS_NAME
            rootfs.write_bytes(b"rootfs\n")
            with (
                mock.patch.object(alpine_builder, "CACHE", cache),
                mock.patch.object(
                    alpine_builder,
                    "_ensure_apk_signing_key",
                    return_value=(Path("private"), Path("public"), "5" * 64),
                ),
                mock.patch.object(alpine_state, "alpine_rootfs_recipe", return_value=recipe),
                mock.patch.object(alpine_state, "receipt_matches", return_value=True),
                mock.patch.object(
                    alpine_builder,
                    "_cached_aport_packages",
                    return_value={package: cached_package},
                ),
            ):
                actual_rootfs, actual_output, actual_recipe, bundle_outputs = (
                    alpine_builder.build_rootfs(1, (package,))
                )
            self.assertEqual(
                (actual_rootfs, actual_output, actual_recipe),
                (rootfs, output, recipe),
            )
            self.assertEqual(bundle_outputs, {})

    def test_bundle_absence_check_interprets_mocked_apk_exit_codes(self) -> None:
        """Map mocked ``apk info --exists`` results to absent and installed outcomes."""
        root = Path(self.temporary.name) / "rootfs"
        package = self.packages[1]
        with mock.patch.object(
            subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 1, "", ""),
        ):
            alpine_builder._require_bundle_package_absent(root, package)  # noqa: SLF001
        with (
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0, f"{package}\n", ""),
            ),
            self.assertRaisesRegex(SystemExit, "installed in the standard rootfs"),
        ):
            alpine_builder._require_bundle_package_absent(root, package)  # noqa: SLF001
        with (
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 2, "", "apk failed"),
            ),
            self.assertRaisesRegex(SystemExit, "cannot verify.*apk failed"),
        ):
            alpine_builder._require_bundle_package_absent(root, package)  # noqa: SLF001

    def test_bundle_install_check_interprets_mocked_apk_simulation(self) -> None:
        """A mocked offline ``apk add --simulate`` result accepts or rejects the bundle APKs."""
        root = self._verified_rootfs()
        base = ("fplinux-base", "fplinux-terminal")
        bundle_apk = self._write("built/fplinux-package-b.apk", b"bundle\n")
        unresolved = (
            "ERROR: unable to select packages:\n"
            "  so:libexample.so.1 (no such package):\n"
            "    required by: fplinux-package-b-1-r0[so:libexample.so.1]\n"
        )
        self._write_world(root, base)

        with (
            mock.patch.object(alpine_builder, "_require_apk_owner"),
            mock.patch.object(
                subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")
            ),
        ):
            alpine_builder._verify_alpine_rootfs(root, base, bundle_apks=(bundle_apk,))  # noqa: SLF001

        with (
            mock.patch.object(alpine_builder, "_require_apk_owner"),
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 7, "", unresolved),
            ),
            self.assertRaises(SystemExit) as rejected,
        ):
            alpine_builder._verify_alpine_rootfs(root, base, bundle_apks=(bundle_apk,))  # noqa: SLF001
        self.assertIn("cannot be installed into the rootfs offline", str(rejected.exception))
        self.assertIn("so:libexample.so.1 (no such package)", str(rejected.exception))

    def _verified_rootfs(self) -> Path:
        """Create the smallest root tree accepted without optional packages."""
        root = Path(self.temporary.name) / "verified-rootfs"
        (root / "etc/init.d").mkdir(parents=True)
        (root / "etc/runlevels/default").mkdir(parents=True)
        (root / "etc/runlevels/boot").mkdir(parents=True)
        (root / "etc/network").mkdir(parents=True)
        (root / "usr/bin").mkdir(parents=True)
        (root / "etc/fstab").write_text(
            "tmpfs\t/tmp\ttmpfs\trw,nosuid,nodev,mode=1777\t0 0\n",
            encoding="utf-8",
        )
        (root / "etc/inittab").write_text("::sysinit:/sbin/openrc sysinit\n", encoding="utf-8")
        (root / "etc/os-release").write_text("NAME=FPLinux\n", encoding="utf-8")
        (root / "etc/network/interfaces").write_text(
            "auto lo\niface lo inet loopback\n", encoding="utf-8"
        )
        (root / "etc/init.d/networking").write_text("#!/bin/sh\n", encoding="utf-8")
        (root / "etc/runlevels/boot/networking").symlink_to("/etc/init.d/networking")
        (root / "etc/init.d/fplinux-terminal").write_text("#!/bin/sh\n", encoding="utf-8")
        (root / "etc/init.d/fplinux-brightness").write_text("#!/bin/sh\n", encoding="utf-8")
        (root / "usr/bin/fplinux-terminal").write_text("terminal\n", encoding="utf-8")
        (root / "usr/bin/fplinux-brightness").write_text("brightness\n", encoding="utf-8")
        (root / "usr/libexec/fplinux").mkdir(parents=True)
        (root / "usr/libexec/fplinux/brightnessd").write_text("brightnessd\n", encoding="utf-8")
        (root / "etc/runlevels/default/fplinux-terminal").symlink_to(
            "/etc/init.d/fplinux-terminal"
        )
        (root / "etc/runlevels/default/fplinux-brightness").symlink_to(
            "/etc/init.d/fplinux-brightness"
        )
        (root / "init").symlink_to("/sbin/init")
        return root

    @staticmethod
    def _write_world(root: Path, packages: tuple[str, ...]) -> None:
        """Select the packages installed into one prepared root tree."""
        (root / "etc/apk").mkdir(parents=True, exist_ok=True)
        (root / "etc/apk/world").write_text(
            "\n".join(packages) + "\n",
            encoding="utf-8",
        )

    def test_brightness_config_has_exact_runtime_bytes_and_is_verified(self) -> None:
        """Composition writes the target table and rejects changed installed bytes."""
        root = self._verified_rootfs()
        packages = ("fplinux-base", "fplinux-terminal")
        self._write_world(root, packages)
        table = {"backlight": "screen-backlight", "levels": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]}
        config = root / "etc/fplinux/brightness.conf"

        alpine_builder._install_display_brightness(root, table)  # noqa: SLF001
        self.assertEqual(
            config.read_bytes(),
            b"backlight=screen-backlight\nlevels=0,1,2,3,4,5,6,7,8,9,10\n",
        )
        self.assertEqual(config.stat().st_mode & 0o777, 0o644)
        with mock.patch.object(alpine_builder, "_require_apk_owner"):
            alpine_builder._verify_alpine_rootfs(  # noqa: SLF001
                root, packages, display_brightness=table
            )
            config.write_text("backlight=other-backlight\nlevels=0,1,2,3,4,5,6,7,8,9,10\n")
            with self.assertRaisesRegex(SystemExit, "brightness configuration does not match"):
                alpine_builder._verify_alpine_rootfs(  # noqa: SLF001
                    root, packages, display_brightness=table
                )

    def test_rootfs_without_brightness_table_has_no_brightness_config(self) -> None:
        """A headless target does not receive display configuration."""
        root = self._verified_rootfs()
        packages = ("fplinux-base", "fplinux-terminal")
        self._write_world(root, packages)
        alpine_builder._install_display_brightness(root, None)  # noqa: SLF001
        config = root / "etc/fplinux/brightness.conf"
        self.assertFalse(config.exists())
        with mock.patch.object(alpine_builder, "_require_apk_owner"):
            alpine_builder._verify_alpine_rootfs(root, packages)  # noqa: SLF001
            config.parent.mkdir(parents=True)
            config.write_text("backlight=unexpected\nlevels=0,1,2,3,4,5,6,7,8,9,10\n")
            with self.assertRaisesRegex(SystemExit, "without a target table"):
                alpine_builder._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    def test_rootfs_requires_brightness_command_and_startup_service(self) -> None:
        """The composed base rootfs must provide the command and its default runlevel."""
        root = self._verified_rootfs()
        packages = ("fplinux-base", "fplinux-terminal")
        self._write_world(root, packages)
        command = root / "usr/bin/fplinux-brightness"
        daemon = root / "usr/libexec/fplinux/brightnessd"
        service = root / "etc/runlevels/default/fplinux-brightness"

        with mock.patch.object(alpine_builder, "_require_apk_owner"):
            command.unlink()
            with self.assertRaisesRegex(SystemExit, "fplinux-brightness"):
                alpine_builder._verify_alpine_rootfs(root, packages)  # noqa: SLF001
            command.write_text("brightness\n", encoding="utf-8")
            daemon.unlink()
            with self.assertRaisesRegex(SystemExit, "brightnessd"):
                alpine_builder._verify_alpine_rootfs(root, packages)  # noqa: SLF001
            daemon.write_text("brightnessd\n", encoding="utf-8")
            service.unlink()
            with self.assertRaisesRegex(SystemExit, "fplinux-brightness"):
                alpine_builder._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    def test_rootfs_verifier_requires_input_files_only_when_selected(self) -> None:
        """A gadgetless root accepts no input bridge, while the selected bridge is required."""
        root = self._verified_rootfs()
        without_input = ("fplinux-base", "fplinux-terminal")
        self._write_world(root, without_input)
        with mock.patch.object(alpine_builder, "_require_apk_owner"):
            alpine_builder._verify_alpine_rootfs(root, without_input)  # noqa: SLF001

        with_input = (*without_input, "fplinux-input")
        self._write_world(root, with_input)
        with (
            mock.patch.object(alpine_builder, "_require_apk_owner"),
            self.assertRaisesRegex(SystemExit, "fplinux-input"),
        ):
            alpine_builder._verify_alpine_rootfs(root, with_input)  # noqa: SLF001

    def test_rootfs_verifier_rejects_disabled_boot_networking(self) -> None:
        """A root lacking network startup must be rejected before publication."""
        root = self._verified_rootfs()
        packages = ("fplinux-base", "fplinux-terminal")
        self._write_world(root, packages)
        with mock.patch.object(alpine_builder, "_require_apk_owner"):
            alpine_builder._verify_alpine_rootfs(root, packages)  # noqa: SLF001
            (root / "etc/runlevels/boot/networking").unlink()
            with self.assertRaisesRegex(SystemExit, "boot runlevel is missing networking"):
                alpine_builder._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    @staticmethod
    def _fake_apk_owner(owners: dict[str, str]) -> Callable[[Path, str, str], None]:
        """Fake apk file-ownership answers; paths outside ``owners`` always match."""

        def fake_apk_owner(_root: Path, path: str, package: str) -> None:
            owner = owners.get(path, package)
            if owner != package:
                raise SystemExit(f"unexpected Alpine package owner for {path}: {owner}")

        return fake_apk_owner

    def test_bluetooth_root_requires_project_daemon_and_library_owners(self) -> None:
        """The selected Bluetooth root rejects stock daemon and GLib replacements."""
        root = self._verified_rootfs()
        base = ("fplinux-base", "fplinux-terminal")
        with_bluetooth = (*base, "fplinux-bluetooth")
        replaced = {
            "/usr/lib/bluetooth/obexd": ("bluez-obexd", "fplinux-bluez"),
            "/usr/lib/libfplinux-bluez-glib-2.0.so.0": ("glib", "fplinux-bluez-glib"),
        }
        project_owners = {path: project for path, (_, project) in replaced.items()}

        self._write_world(root, base)
        alpine_owners = {path: alpine for path, (alpine, _) in replaced.items()}
        with mock.patch.object(
            alpine_builder, "_require_apk_owner", self._fake_apk_owner(alpine_owners)
        ):
            alpine_builder._verify_alpine_rootfs(root, base)  # noqa: SLF001

        self._write_world(root, with_bluetooth)
        for path, (alpine, _) in replaced.items():
            with self.subTest(path=path):
                owners = {**project_owners, path: alpine}
                with (
                    mock.patch.object(
                        alpine_builder, "_require_apk_owner", self._fake_apk_owner(owners)
                    ),
                    self.assertRaisesRegex(SystemExit, f"{re.escape(path)}: {alpine}"),
                ):
                    alpine_builder._verify_alpine_rootfs(root, with_bluetooth)  # noqa: SLF001

        with mock.patch.object(
            alpine_builder, "_require_apk_owner", self._fake_apk_owner(project_owners)
        ):
            alpine_builder._verify_alpine_rootfs(root, with_bluetooth)  # noqa: SLF001

    def test_replaced_package_manager_root_rejects_openssl_leftovers(self) -> None:
        """A root with the Mbed TLS apk must own /sbin/apk and hold no OpenSSL packages."""
        root = self._verified_rootfs()
        base = ("fplinux-base", "fplinux-terminal")
        with_apk_tools = (*base, "fplinux-apk-tools")
        installed: set[str] = {"apk-tools", "libapk", "libcrypto3", "libssl3", "ssl_client"}
        minirootfs_owner = self._fake_apk_owner({"/sbin/apk": "apk-tools"})

        def fake_installed(_root: Path, package: str) -> bool:
            return package in installed

        self._write_world(root, base)
        with (
            mock.patch.object(alpine_builder, "_require_apk_owner", minirootfs_owner),
            mock.patch.object(alpine_builder, "_alpine_package_installed", fake_installed),
        ):
            alpine_builder._verify_alpine_rootfs(root, base)  # noqa: SLF001

        self._write_world(root, with_apk_tools)
        with (
            mock.patch.object(alpine_builder, "_require_apk_owner", minirootfs_owner),
            mock.patch.object(alpine_builder, "_alpine_package_installed", fake_installed),
            self.assertRaisesRegex(SystemExit, "/sbin/apk: apk-tools"),
        ):
            alpine_builder._verify_alpine_rootfs(root, with_apk_tools)  # noqa: SLF001

        for leftover in ("apk-tools", "libapk", "libcrypto3", "libssl3"):
            with self.subTest(leftover=leftover):
                installed = {leftover}
                with (
                    mock.patch.object(
                        alpine_builder, "_require_apk_owner", self._fake_apk_owner({})
                    ),
                    mock.patch.object(alpine_builder, "_alpine_package_installed", fake_installed),
                    self.assertRaisesRegex(SystemExit, f"remains in the rootfs: {leftover}"),
                ):
                    alpine_builder._verify_alpine_rootfs(root, with_apk_tools)  # noqa: SLF001

        installed = set()
        with (
            mock.patch.object(alpine_builder, "_require_apk_owner", self._fake_apk_owner({})),
            mock.patch.object(alpine_builder, "_alpine_package_installed", fake_installed),
        ):
            alpine_builder._verify_alpine_rootfs(root, with_apk_tools)  # noqa: SLF001

    def test_persistent_root_requires_orderly_shutdown_services(self) -> None:
        """A persistent root rejects a shutdown runlevel missing data-safety services."""
        root = self._verified_rootfs()
        without_input = ("fplinux-base", "fplinux-terminal")
        microsd_root = (*without_input, "fplinux-microsd-root")
        self._write_world(root, microsd_root)
        with (
            mock.patch.object(alpine_builder, "_require_apk_owner"),
            self.assertRaisesRegex(SystemExit, "shutdown runlevel is missing killprocs"),
        ):
            alpine_builder._verify_alpine_rootfs(root, microsd_root)  # noqa: SLF001
        shutdown = root / "etc/runlevels/shutdown"
        shutdown.mkdir(parents=True)
        for service in ("killprocs", "savecache", "mount-ro"):
            (shutdown / service).symlink_to(f"/etc/init.d/{service}")
        with mock.patch.object(alpine_builder, "_require_apk_owner"):
            alpine_builder._verify_alpine_rootfs(root, microsd_root)  # noqa: SLF001

    def test_signing_key_identity_requires_one_regular_public_key(self) -> None:
        """Keep package signing state explicit rather than silently generating it in prune."""
        cache = Path(self.temporary.name) / "cache"
        key = alpine_state.signing_public_key(cache)
        with self.assertRaisesRegex(SystemExit, "signing public key"):
            alpine_state.signing_key_identity(cache)
        key.parent.mkdir(parents=True)
        key.write_bytes(b"public-key\n")
        self.assertEqual(
            alpine_state.signing_key_identity(cache),
            hashlib.sha256(key.read_bytes()).hexdigest(),
        )

    def test_bootstrap_change_invalidates_ram_receipt_but_not_external_root(self) -> None:
        """RAM reuse follows bootstrap bytes while storage composition stays reusable."""
        ram_recipe = self._recipe()
        external_recipe = self._recipe(root_kind="external")
        cache = Path(self.temporary.name) / "cache"
        ram_output = alpine_state.rootfs_output(cache, ram_recipe)
        external_output = alpine_state.rootfs_output(cache, external_recipe)
        for output in (ram_output, external_output):
            output.mkdir(parents=True)
            (output / "rootfs.cpio").write_bytes(b"logical composition\n")
        (ram_output / "initramfs.cpio").write_bytes(b"boot archive\n")
        alpine_state.write_receipt(ram_output, ram_recipe)
        alpine_state.write_receipt(external_output, external_recipe)
        ram_receipt = json.loads((ram_output / alpine_state.RECEIPT_NAME).read_text())
        external_receipt = json.loads((external_output / alpine_state.RECEIPT_NAME).read_text())
        self.assertEqual(
            ram_receipt,
            {
                "recipe": ram_recipe,
                "rootfs": {
                    "size": 20,
                    "sha256": hashlib.sha256(b"logical composition\n").hexdigest(),
                },
                "initramfs": {
                    "size": 13,
                    "sha256": hashlib.sha256(b"boot archive\n").hexdigest(),
                },
            },
        )
        self.assertIsNone(external_receipt["initramfs"])
        self.assertTrue(alpine_state.receipt_matches(ram_output, self._recipe()))
        self.assertTrue(
            alpine_state.receipt_matches(external_output, self._recipe(root_kind="external"))
        )
        self._write("unrelated.txt", b"unrelated edit\n")
        self.assertTrue(alpine_state.receipt_matches(ram_output, self._recipe()))
        self.bootstrap.write_bytes(b"#!/bin/sh\nexit 1\n")
        changed = self._recipe()
        self.assertFalse(alpine_state.receipt_matches(ram_output, changed))
        self.assertTrue(
            alpine_state.receipt_matches(external_output, self._recipe(root_kind="external"))
        )
        rebuilt = alpine_state.rootfs_output(cache, changed)
        rebuilt.mkdir(parents=True)
        (rebuilt / "rootfs.cpio").write_bytes(b"logical composition\n")
        (rebuilt / "initramfs.cpio").write_bytes(b"changed boot archive\n")
        alpine_state.write_receipt(rebuilt, changed)
        self.assertTrue(alpine_state.receipt_matches(rebuilt, changed))
        self.assertNotEqual(
            alpine_state.trusted_receipt_identity(ram_output, ram_recipe),
            alpine_state.trusted_receipt_identity(rebuilt, changed),
        )

    def test_receipt_matches_only_exact_rootfs_bytes(self) -> None:
        """A successful receipt is revoked by any rootfs byte change."""
        recipe = self._recipe()
        cache = Path(self.temporary.name) / "cache"
        output = alpine_state.rootfs_output(cache, recipe)
        output.mkdir(parents=True)
        rootfs = output / alpine_state.ROOTFS_NAME
        rootfs.write_bytes(b"rootfs\n")
        (output / "initramfs.cpio").write_bytes(b"boot archive\n")
        alpine_state.write_receipt(output, recipe)

        self.assertTrue(alpine_state.receipt_matches(output, recipe))
        identity = alpine_state.trusted_receipt_identity(output, recipe)
        self.assertEqual(identity["recipe"], recipe)

        rootfs.write_bytes(b"tampered\n")
        self.assertFalse(alpine_state.receipt_matches(output, recipe))

    def test_receipt_with_unknown_shape_is_a_cache_miss(self) -> None:
        """An unrecognized cache artifact is ignored without migration."""
        recipe = self._recipe()
        output = Path(self.temporary.name) / "output"
        output.mkdir()
        (output / alpine_state.ROOTFS_NAME).write_bytes(b"rootfs\n")
        (output / "initramfs.cpio").write_bytes(b"boot archive\n")
        alpine_state.write_receipt(output, recipe)
        receipt_path = output / alpine_state.RECEIPT_NAME
        receipt = json.loads(receipt_path.read_text())
        receipt["unknown"] = "receipt-field"
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")

        self.assertFalse(alpine_state.receipt_matches(output, recipe))


if __name__ == "__main__":
    unittest.main()
