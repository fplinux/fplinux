# SPDX-License-Identifier: GPL-2.0-only
"""Focused tests for the one content-addressed Alpine rootfs state."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import alpine_builder, alpine_state
from fplinux_cli.common import sha256_file


class AlpineStateTests(unittest.TestCase):
    """Keep selected Alpine inputs, outputs and receipts exact."""

    def setUp(self) -> None:
        """Create one complete minimal Alpine rootfs recipe fixture."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        self.signing_key = "d" * 64
        self.packages = tuple(sorted((*alpine_state.COMMON_PACKAGES, "fplinux-cpuclock")))
        self._write(
            "alpine.lock.toml",
            b'schema = "fplinux.alpine/v1"\n'
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
        self.aport = self.root / "alpine/aports/fplinux-base/APKBUILD"
        self._write("alpine/aports/not-production/APKBUILD", b"pkgname=not-production\n")
        self._write("scripts/fplinux_cli/alpine_state.py", b"state implementation\n")
        self._write("scripts/fplinux_cli/builder.py", b"builder implementation\n")
        self._write("scripts/fplinux_cli/alpine_builder.py", b"Alpine builder implementation\n")

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

    def test_selection_combines_common_platform_and_target_packages(self) -> None:
        """The current UMS9117 layer owns cpuclock while the target adds nothing."""
        selected = alpine_state.selected_packages(
            {"rootfs": {"packages": ["fplinux-cpuclock"]}},
            {"rootfs": {"packages": []}},
            self.root,
        )
        self.assertEqual(selected, self.packages)

    def test_selection_rejects_duplicate_ownership(self) -> None:
        """One package cannot be owned by both common and platform layers."""
        with self.assertRaisesRegex(SystemExit, "owned by both common and platform"):
            alpine_state.selected_packages(
                {"rootfs": {"packages": ["fplinux-base"]}},
                {"rootfs": {"packages": []}},
                self.root,
            )

    def test_same_selected_set_shares_one_rootfs_recipe(self) -> None:
        """Ownership declaration order does not split identical rootfs content."""
        platform_owned = alpine_state.selected_packages(
            {"rootfs": {"packages": ["fplinux-cpuclock"]}},
            {"rootfs": {"packages": []}},
            self.root,
        )
        target_owned = alpine_state.selected_packages(
            {"rootfs": {"packages": []}},
            {"rootfs": {"packages": ["fplinux-cpuclock"]}},
            self.root,
        )
        self.assertEqual(platform_owned, target_owned)
        self.assertEqual(
            self._recipe(packages=platform_owned),
            self._recipe(packages=target_owned),
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

    def test_selected_aport_bytes_change_the_rootfs_recipe(self) -> None:
        """A selected package-source edit invalidates the shared rootfs."""
        first = self._recipe()
        self.aport.write_bytes(b"pkgname=fplinux-base\npkgrel=1\n")
        second = self._recipe()
        self.assertNotEqual(first, second)
        self.assertEqual(second, self._recipe())

    def test_kernel_only_builder_bytes_do_not_change_alpine_recipes(self) -> None:
        """Kernel-only builder edits cannot invalidate the Alpine rootfs or APK slots."""
        rootfs_before = self._recipe()
        package_before = alpine_state.alpine_package_recipe(
            "fplinux-base", "1" * 64, self.signing_key, self.root
        )
        self._write("scripts/fplinux_cli/builder.py", b"kernel implementation changed\n")
        self.assertEqual(rootfs_before, self._recipe())
        self.assertEqual(
            package_before,
            alpine_state.alpine_package_recipe(
                "fplinux-base", "1" * 64, self.signing_key, self.root
            ),
        )

    def test_alpine_builder_bytes_change_alpine_recipes(self) -> None:
        """The selected rootfs and APK implementation remains a causal input."""
        rootfs_before = self._recipe()
        package_before = alpine_state.alpine_package_recipe(
            "fplinux-base", "1" * 64, self.signing_key, self.root
        )
        self._write("scripts/fplinux_cli/alpine_builder.py", b"Alpine builder changed\n")
        self.assertNotEqual(rootfs_before, self._recipe())
        self.assertNotEqual(
            package_before,
            alpine_state.alpine_package_recipe(
                "fplinux-base", "1" * 64, self.signing_key, self.root
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
        cpuclock = self.root / "alpine/aports/fplinux-cpuclock/APKBUILD"
        cpuclock.write_bytes(b"pkgname=fplinux-cpuclock\npkgrel=1\n")
        after = {
            name: alpine_state.alpine_package_recipe(
                name,
                "1" * 64,
                self.signing_key,
                self.root,
            )
            for name in self.packages
        }

        self.assertNotEqual(before["fplinux-cpuclock"], after["fplinux-cpuclock"])
        for name in set(self.packages) - {"fplinux-cpuclock"}:
            self.assertEqual(before[name], after[name])

    def test_unselected_aport_is_not_causal(self) -> None:
        """Files outside the selected package set cannot invalidate its rootfs."""
        first = self._recipe()
        self._write("alpine/aports/not-production/APKBUILD", b"pkgrel=9\n")
        self.assertEqual(first, self._recipe())

    def test_package_set_is_causal(self) -> None:
        """Removing one otherwise valid package selects a different rootfs."""
        without_cpuclock = tuple(
            package for package in self.packages if package != "fplinux-cpuclock"
        )
        self.assertNotEqual(self._recipe(), self._recipe(packages=without_cpuclock))

    def test_rootfs_world_requires_the_exact_selected_package_set(self) -> None:
        """Composition cannot silently omit or add a local FPLinux package."""
        rootfs = Path(self.temporary.name) / "rootfs"
        rootfs.mkdir()
        (rootfs / "init").symlink_to("/sbin/init")
        contents = {
            "etc/fstab": "tmpfs\t/tmp\ttmpfs\trw,nosuid,nodev,mode=1777\t0 0\n",
            "etc/inittab": "# OpenRC owns interactive services\n",
            "etc/os-release": "NAME=FPLinux\n",
            "etc/init.d/fplinux-console": "",
            "etc/init.d/fplinux-usb-getty": "",
            "etc/init.d/fplinux-input": "",
            "usr/bin/fplinux-console": "",
            "usr/bin/fplinux-input": "",
            "usr/bin/fplinux-cpuclock": "",
            "usr/bin/quake": "",
            "usr/bin/tyr-quake": "",
        }
        for relative, contents_text in contents.items():
            destination = rootfs / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(contents_text, encoding="utf-8")
        for service in ("fplinux-input", "fplinux-usb-getty", "fplinux-console"):
            link = rootfs / "etc/runlevels/default" / service
            link.parent.mkdir(parents=True, exist_ok=True)
            link.symlink_to(f"/etc/init.d/{service}")
        world = rootfs / "etc/apk/world"
        world.parent.mkdir(parents=True, exist_ok=True)
        world.write_text("busybox\n" + "\n".join(self.packages) + "\n", encoding="utf-8")

        with mock.patch.object(alpine_builder, "_require_apk_owner") as require_owner:
            alpine_builder._verify_alpine_rootfs(rootfs, self.packages)  # noqa: SLF001
        self.assertIn(
            mock.call(rootfs, "/usr/bin/fplinux-cpuclock", "fplinux-cpuclock"),
            require_owner.call_args_list,
        )

        world.write_text("\n".join(self.packages[:-1]) + "\n", encoding="utf-8")
        with (
            mock.patch.object(alpine_builder, "_require_apk_owner"),
            self.assertRaisesRegex(SystemExit, "exact selected FPLinux package set"),
        ):
            alpine_builder._verify_alpine_rootfs(rootfs, self.packages)  # noqa: SLF001

    def test_rootfs_install_command_adds_the_exact_selected_package_set(self) -> None:
        """Composition requests each selected package by name and no implicit meta-root."""
        command = alpine_builder._rootfs_install_command(  # noqa: SLF001
            {"arch": "armv7"},
            Path("/rootfs"),
            Path("/keys"),
            Path("/repository"),
            self.packages,
        )
        self.assertEqual(
            command,
            [
                "apk",
                "--root",
                "/rootfs",
                "--arch",
                "armv7",
                "--no-network",
                "--no-scripts",
                "--no-logfile",
                "--keys-dir",
                "/keys",
                "--repositories-file",
                "/dev/null",
                "--repository",
                "/repository",
                "add",
                *self.packages,
            ],
        )

    def test_container_image_identity_is_causal(self) -> None:
        """Changing the build environment invalidates the rootfs recipe."""
        self.assertNotEqual(self._recipe("1" * 64), self._recipe("2" * 64))

    def test_package_signing_key_is_causal(self) -> None:
        """A different persistent abuild key must produce a different recipe."""
        self.assertNotEqual(self._recipe(signing_key="d" * 64), self._recipe(signing_key="e" * 64))

    def test_fixed_apk_slots_rebuild_only_changed_aport_and_keep_last_good(self) -> None:
        """Normal repeat/edit/failure calls reuse exact APKs without losing a good slot."""
        cache = Path(self.temporary.name) / "cache"
        sources = cache / "downloads/alpine/sources"
        sources.mkdir(parents=True)
        private_key = self._write("keys/fplinux-build.rsa", b"private key\n")
        public_key = self._write("keys/fplinux-build.rsa.pub", b"public key\n")
        builds: list[str] = []
        failing_package: str | None = None
        original_recipe = alpine_state.alpine_package_recipe

        def package_recipe(name: str, image: str, signing_key: str) -> str:
            return original_recipe(name, image, signing_key, root=self.root)

        def list_packages(command: list[str], cwd: Path, environment: dict[str, str]) -> str:
            self.assertEqual(command, ["abuild", "listpkg"])
            self.assertIn("REPODEST", environment)
            return f"{cwd.name}-1.0-r0.apk\n"

        def run_as_builder(command: list[str], cwd: Path, environment: dict[str, str]) -> None:
            nonlocal failing_package
            if command == ["apkbuild-lint", "APKBUILD"]:
                return
            self.assertEqual(command, ["abuild", "-d", "-r"])
            if cwd.name == failing_package:
                raise SystemExit(f"build failed: abuild failed for {cwd.name}")
            builds.append(cwd.name)
            output = Path(environment["REPODEST"])
            output.mkdir(parents=True, exist_ok=True)
            (output / f"{cwd.name}-1.0-r0.apk").write_text(f"{cwd.name}\n", encoding="utf-8")

        invocation = 0

        def build_apks() -> tuple[list[Path], Path, Path]:
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
            mock.patch.object(alpine_builder, "_log_message"),
            mock.patch.object(alpine_state, "alpine_package_recipe", side_effect=package_recipe),
        ):
            build_apks()
            self.assertEqual(builds, list(self.packages))

            builds.clear()
            build_apks()
            self.assertEqual(builds, [])

            self._write(
                "alpine/aports/fplinux-cpuclock/APKBUILD",
                b"pkgname=fplinux-cpuclock\npkgrel=1\n",
            )
            build_apks()
            self.assertEqual(builds, ["fplinux-cpuclock"])

            slot = cache / alpine_state.PACKAGE_CACHE_DIRECTORY / "fplinux-cpuclock"
            marker = slot / alpine_state.PACKAGE_RECIPE_NAME
            package = slot / "fplinux-cpuclock-1.0-r0.apk"
            previous_marker = marker.read_bytes()
            previous_package = package.read_bytes()
            self._write(
                "alpine/aports/fplinux-cpuclock/APKBUILD",
                b"pkgname=fplinux-cpuclock\npkgrel=2\n",
            )
            failing_package = "fplinux-cpuclock"
            with self.assertRaisesRegex(SystemExit, "abuild failed for fplinux-cpuclock"):
                build_apks()

        self.assertEqual(marker.read_bytes(), previous_marker)
        self.assertEqual(package.read_bytes(), previous_package)

    def test_signing_key_identity_requires_one_regular_public_key(self) -> None:
        """Keep package signing state explicit rather than silently generating it in prune."""
        cache = Path(self.temporary.name) / "cache"
        key = alpine_state.signing_public_key(cache)
        with self.assertRaisesRegex(SystemExit, "signing public key"):
            alpine_state.signing_key_identity(cache)
        key.parent.mkdir(parents=True)
        key.write_bytes(b"public-key\n")
        self.assertEqual(alpine_state.signing_key_identity(cache), sha256_file(key))

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
        self.assertEqual(set(identity), {"recipe", "sha256"})

        rootfs.write_bytes(b"tampered\n")
        self.assertFalse(alpine_state.receipt_matches(output, recipe))

    def test_receipt_contains_only_causal_fields(self) -> None:
        """Receipt format is exactly its current causal identity and output record."""
        recipe = self._recipe()
        output = Path(self.temporary.name) / "output"
        output.mkdir()
        (output / alpine_state.ROOTFS_NAME).write_bytes(b"rootfs\n")
        alpine_state.write_receipt(output, recipe)
        receipt = json.loads((output / alpine_state.RECEIPT_NAME).read_text())
        self.assertEqual(set(receipt), {"recipe", "rootfs"})

    def test_receipt_with_unknown_shape_is_a_cache_miss(self) -> None:
        """Old or unrecognized cache artifacts are ignored without migration."""
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


class AlpineSourceCacheTests(unittest.TestCase):
    """Keep downloaded aport sources across ordinary rootfs rebuilds."""

    def test_source_cache_is_outside_disposable_rootfs_work(self) -> None:
        """Two rebuilds reuse the same Alpine source download directory."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / "cache"
            with (
                mock.patch.object(alpine_builder, "CACHE", cache),
                mock.patch.object(alpine_builder, "_chown_tree") as chown_tree,
            ):
                first = alpine_builder._alpine_source_cache()  # noqa: SLF001
                archive = first / "tyrquake-0.71.tar.gz"
                archive.write_bytes(b"locked source archive\n")
                second = alpine_builder._alpine_source_cache()  # noqa: SLF001

            self.assertEqual(first, cache / "downloads/alpine/sources")
            self.assertEqual(second, first)
            self.assertEqual(archive.read_bytes(), b"locked source archive\n")
            self.assertEqual(chown_tree.call_args_list, [mock.call(first, "builder")] * 2)


if __name__ == "__main__":
    unittest.main()
