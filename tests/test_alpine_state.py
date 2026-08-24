# SPDX-License-Identifier: GPL-2.0-only
"""Focused tests for the one content-addressed Alpine rootfs state."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import alpine_builder, alpine_state


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
            b'repository = "https://example.invalid/alpine/v3.24/main"\n'
            b"\n[minirootfs]\n"
            b'url = "https://example.invalid/alpine-minirootfs.tar.gz"\n'
            b'sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"\n'
            b"bytes = 1\n"
            b"\n[runtime]\n"
            b'packages = ["openrc-1-r0.apk"]\n'
            b"\n[sysroot]\n"
            b'packages = ["musl-dev-1-r0.apk"]\n'
            b"\n[[package]]\n"
            b'file = "openrc-1-r0.apk"\n'
            b'sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"\n'
            b"bytes = 2\n"
            b"\n[[package]]\n"
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
        self._write("scripts/fplinux_cli/builder.py", b"builder implementation\n")
        self._write("scripts/fplinux_cli/alpine_builder.py", b"Alpine builder implementation\n")
        self.shared_source = self._write("alpine/shared/shared.c", b"int shared;\n")

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
    ) -> str:
        return alpine_state.alpine_rootfs_recipe(
            image,
            self.signing_key if signing_key is None else signing_key,
            self.packages if packages is None else packages,
            self.root,
        )

    def test_selection_combines_common_and_platform_ownership(self) -> None:
        """Common and platform ownership contribute one canonical rootfs set."""
        third = "fplinux-package-c"
        self._write(f"alpine/aports/{third}/APKBUILD", f"pkgname={third}\n".encode())
        with mock.patch.object(alpine_state, "COMMON_PACKAGES", (self.packages[0],)):
            selected = alpine_state.selected_packages(
                {"rootfs": {"packages": [self.packages[1], third]}},
                {},
                self.root,
            )
        self.assertEqual(selected, (*self.packages, third))

    def test_package_cannot_be_selected_and_bundle_published(self) -> None:
        """One package cannot be both installed and published separately."""
        with self.assertRaisesRegex(SystemExit, "both rootfs-selected and bundle-published"):
            alpine_state.bundle_packages(
                {"bundle": {"packages": [self.packages[0]]}},
                {"bundle": {"packages": []}},
                self.packages,
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
                {"rootfs": {"packages": []}},
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
                        "packages": [extra],
                        "exclude_packages": [self.packages[1]],
                    }
                },
                self.root,
            )
        self.assertEqual(selected, (self.packages[0], extra))

    def test_profile_can_exclude_gadget_input_stack_and_retain_console(self) -> None:
        """A host-only rootfs removes gadget-only input without losing the console."""
        common = ("fplinux-base", "fplinux-console", "fplinux-input")
        platform = ("fplinux-usb-gadget", "fplinux-ssh")
        for package in (*common, *platform):
            self._write(f"alpine/aports/{package}/APKBUILD", f"pkgname={package}\n".encode())

        with mock.patch.object(alpine_state, "COMMON_PACKAGES", common):
            selected = alpine_state.selected_packages(
                {"rootfs": {"packages": list(platform)}},
                {
                    "rootfs": {
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

        self.assertEqual(selected, ("fplinux-base", "fplinux-console"))

    def test_profile_rootfs_rejects_unknown_excludes_and_duplicate_additions(self) -> None:
        """A profile cannot silently remove or repeat an unowned rootfs package."""
        with mock.patch.object(alpine_state, "COMMON_PACKAGES", (self.packages[0],)):
            with self.assertRaisesRegex(SystemExit, "excludes a package not owned"):
                alpine_state.selected_packages(
                    {"rootfs": {"packages": [self.packages[1]]}},
                    {"rootfs": {"packages": [], "exclude_packages": ["fplinux-missing"]}},
                    self.root,
                )
            with self.assertRaisesRegex(SystemExit, "duplicate common/platform ownership"):
                alpine_state.selected_packages(
                    {"rootfs": {"packages": [self.packages[1]]}},
                    {"rootfs": {"packages": [self.packages[1]], "exclude_packages": []}},
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
        self._write("scripts/fplinux_cli/builder.py", b"kernel implementation changed\n")
        self.assertEqual(rootfs_before, self._recipe())
        self.assertEqual(
            package_before,
            alpine_state.alpine_package_recipe(
                self.packages[0], "1" * 64, self.signing_key, self.root
            ),
        )

    def test_alpine_builder_bytes_change_alpine_recipes(self) -> None:
        """The selected rootfs and APK implementation remains a causal input."""
        rootfs_before = self._recipe()
        package_before = alpine_state.alpine_package_recipe(
            self.packages[0], "1" * 64, self.signing_key, self.root
        )
        self._write("scripts/fplinux_cli/alpine_builder.py", b"Alpine builder changed\n")
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

    def test_unselected_aport_is_not_causal(self) -> None:
        """Files outside the selected package set cannot invalidate its rootfs."""
        first = self._recipe()
        self._write("alpine/aports/not-production/APKBUILD", b"pkgrel=9\n")
        self.assertEqual(first, self._recipe())

    def test_package_set_is_causal(self) -> None:
        """Removing one otherwise valid package selects a different rootfs."""
        self.assertNotEqual(self._recipe(), self._recipe(packages=self.packages[:1]))

    def test_container_image_identity_is_causal(self) -> None:
        """Changing the build environment invalidates the rootfs recipe."""
        self.assertNotEqual(self._recipe("1" * 64), self._recipe("2" * 64))

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
        original_recipe = alpine_state.alpine_package_recipe

        def package_recipe(name: str, image: str, signing_key: str) -> str:
            return original_recipe(name, image, signing_key, root=self.root)

        def list_packages(command: list[str], cwd: Path, environment: dict[str, str]) -> str:
            del command, environment
            return f"{cwd.name}-1.0-r0.apk\n"

        def run_as_builder(command: list[str], cwd: Path, environment: dict[str, str]) -> None:
            nonlocal failing_package
            if command == ["apkbuild-lint", "APKBUILD"]:
                return
            if cwd.name == failing_package:
                raise SystemExit(f"build failed: abuild failed for {cwd.name}")
            builds.append(cwd.name)
            output = Path(environment["REPODEST"])
            output.mkdir(parents=True, exist_ok=True)
            (output / f"{cwd.name}-1.0-r0.apk").write_text(f"{cwd.name}\n", encoding="utf-8")

        invocation = 0

        def build_apks() -> tuple[dict[str, Path], Path, Path]:
            nonlocal invocation
            work = Path(self.temporary.name) / f"work-{invocation}"
            invocation += 1
            work.mkdir()
            return alpine_builder._build_fplinux_apks(  # noqa: SLF001
                {"triplet": "armv7-alpine-linux-musleabihf"},
                Path(self.temporary.name) / "sysroot",
                work,
                1,
                private_key,
                public_key,
                self.packages,
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
        ):
            build_apks()
            self.assertEqual(builds, list(self.packages))

            builds.clear()
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

    def test_rootfs_hit_rebuilds_a_missing_bundle_without_recomposing_rootfs(self) -> None:
        """A missing bundle APK is recovered while the last-good rootfs remains selected."""
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

        with (
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
                side_effect=({rootfs_package: base_apk}, None),
            ),
            mock.patch.object(alpine_state, "load_alpine_lock", return_value=lock),
            mock.patch.object(alpine_state, "package_records", return_value={}),
            mock.patch.object(alpine_builder, "_fetch", return_value=archive),
            mock.patch.object(
                alpine_builder,
                "_alpine_group_packages",
                side_effect=([], []),
            ),
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
        ):
            actual_rootfs, actual_output, recipe, bundle_outputs = alpine_builder.build_rootfs(
                2,
                (rootfs_package,),
                (bundle_package,),
            )

        self.assertEqual((actual_rootfs, actual_output, recipe), (rootfs, output, "9" * 64))
        self.assertEqual(bundle_outputs, {bundle_package: bundle_apk})

    def test_rootfs_recipes_share_one_fixed_build_lock(self) -> None:
        """Rootfs cache hits for distinct recipes leave only the shared build lock."""
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

        locks = sorted(path.name for path in (cache / "rootfs").glob(".*.lock"))
        self.assertEqual(locks, [".build.lock"])
        for recipe in recipes:
            self.assertFalse((cache / "rootfs" / f".{recipe}.lock").exists())

    def test_rootfs_build_refuses_a_symlinked_cache_root(self) -> None:
        """An unsafe rootfs-cache link cannot redirect a build into external state."""
        cache = Path(self.temporary.name) / "cache"
        cache.mkdir()
        external = Path(self.temporary.name) / "external"
        external.mkdir()
        sentinel = external / "sentinel"
        sentinel.write_bytes(b"keep\n")
        (cache / "rootfs").symlink_to(external, target_is_directory=True)

        with (
            mock.patch.object(alpine_builder, "CACHE", cache),
            mock.patch.object(
                alpine_builder,
                "_ensure_apk_signing_key",
                return_value=(Path("private"), Path("public"), "5" * 64),
            ),
            mock.patch.object(alpine_state, "alpine_rootfs_recipe", return_value="6" * 64),
            self.assertRaisesRegex(SystemExit, "rootfs cache directory is missing or invalid"),
        ):
            alpine_builder.build_rootfs(1, (self.packages[0],))

        self.assertTrue((cache / "rootfs").is_symlink())
        self.assertEqual(sentinel.read_bytes(), b"keep\n")

    def test_rootfs_build_refuses_a_symlinked_or_nonregular_lock(self) -> None:
        """The fixed lock cannot redirect to or become another cache object."""
        cache = Path(self.temporary.name) / "cache"
        rootfs = cache / "rootfs"
        rootfs.mkdir(parents=True)
        external = Path(self.temporary.name) / "external"
        external.mkdir()
        sentinel = external / "sentinel"
        sentinel.write_bytes(b"keep\n")
        lock = rootfs / ".build.lock"
        lock.symlink_to(sentinel)

        with (
            mock.patch.object(alpine_builder, "CACHE", cache),
            mock.patch.object(
                alpine_builder,
                "_ensure_apk_signing_key",
                return_value=(Path("private"), Path("public"), "5" * 64),
            ),
            mock.patch.object(alpine_state, "alpine_rootfs_recipe", return_value="7" * 64),
            self.assertRaisesRegex(SystemExit, "rootfs build lock is missing or invalid"),
        ):
            alpine_builder.build_rootfs(1, (self.packages[0],))

        self.assertEqual(sentinel.read_bytes(), b"keep\n")
        lock.unlink()
        lock.mkdir()
        with self.assertRaisesRegex(SystemExit, "rootfs build lock is missing or invalid"):
            alpine_builder._open_rootfs_build_lock(rootfs)  # noqa: SLF001

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

    def test_rootfs_verifier_makes_input_files_and_service_conditional(self) -> None:
        """A gadgetless rootfs needs the console, while a selected input bridge is required."""
        root = Path(self.temporary.name) / "verified-rootfs"
        (root / "etc/init.d").mkdir(parents=True)
        (root / "etc/runlevels/default").mkdir(parents=True)
        (root / "usr/bin").mkdir(parents=True)
        (root / "etc/fstab").write_text(
            "tmpfs\t/tmp\ttmpfs\trw,nosuid,nodev,mode=1777\t0 0\n",
            encoding="utf-8",
        )
        (root / "etc/inittab").write_text("::sysinit:/sbin/openrc sysinit\n", encoding="utf-8")
        (root / "etc/os-release").write_text("NAME=FPLinux\n", encoding="utf-8")
        (root / "etc/init.d/fplinux-console").write_text("#!/bin/sh\n", encoding="utf-8")
        (root / "usr/bin/fplinux-console").write_text("console\n", encoding="utf-8")
        (root / "etc/runlevels/default/fplinux-console").symlink_to("/etc/init.d/fplinux-console")
        (root / "init").symlink_to("/sbin/init")

        def write_world(packages: tuple[str, ...]) -> None:
            (root / "etc/apk").mkdir(parents=True, exist_ok=True)
            (root / "etc/apk/world").write_text(
                "\n".join(packages) + "\n",
                encoding="utf-8",
            )

        without_input = ("fplinux-base", "fplinux-console")
        write_world(without_input)
        with mock.patch.object(alpine_builder, "_require_apk_owner") as owners:
            alpine_builder._verify_alpine_rootfs(root, without_input)  # noqa: SLF001
        self.assertNotIn(
            mock.call(root, "/usr/bin/fplinux-input", "fplinux-input"), owners.call_args_list
        )

        with_input = (*without_input, "fplinux-input")
        write_world(with_input)
        with (
            mock.patch.object(alpine_builder, "_require_apk_owner"),
            self.assertRaisesRegex(SystemExit, "fplinux-input"),
        ):
            alpine_builder._verify_alpine_rootfs(root, with_input)  # noqa: SLF001

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

    def test_receipt_matches_only_exact_rootfs_bytes(self) -> None:
        """A successful receipt is revoked by any rootfs byte change."""
        recipe = self._recipe()
        cache = Path(self.temporary.name) / "cache"
        output = alpine_state.rootfs_output(cache, recipe)
        output.mkdir(parents=True)
        rootfs = output / alpine_state.ROOTFS_NAME
        rootfs.write_bytes(b"rootfs\n")
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
        alpine_state.write_receipt(output, recipe)
        receipt_path = output / alpine_state.RECEIPT_NAME
        receipt = json.loads(receipt_path.read_text())
        receipt["unknown"] = "receipt-field"
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")

        self.assertFalse(alpine_state.receipt_matches(output, recipe))


if __name__ == "__main__":
    unittest.main()
