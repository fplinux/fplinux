# SPDX-License-Identifier: GPL-2.0-only
"""Alpine cache scenarios."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import tarfile
from contextlib import nullcontext
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.alpine import (
    aports as alpine_aports,
)
from fplinux_cli.alpine import (
    lock as alpine_lock,
)
from fplinux_cli.alpine import (
    packages as alpine_packages,
)
from fplinux_cli.alpine import (
    recipes as alpine_recipes,
)
from fplinux_cli.alpine import (
    registration as alpine_registration,
)
from fplinux_cli.alpine import (
    rootfs as alpine_rootfs,
)
from fplinux_cli.alpine import (
    rootfs_state as alpine_rootfs_state,
)
from fplinux_cli.alpine import (
    selection as alpine_selection,
)
from fplinux_cli.alpine import (
    signing as alpine_signing,
)
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build

from tests.small.alpine import fixtures


class AlpineCacheTests(fixtures.AlpineSourceFixture):
    """Protect Alpine cache behavior with controlled temporary inputs."""

    def test_subpackage_rootfs_receipt_tracks_output_selection_and_producer_changes(self) -> None:
        """The chosen child is causal, producer edits miss, and unrelated edits still hit."""
        producer = "fplinux-ncurses"
        child = "fplinux-ncurses-curses"
        aport = self._write(f"alpine/aports/{producer}/APKBUILD", b"pkgname=fplinux-ncurses\n")
        output = self.root / "built-rootfs"
        output.mkdir()
        (output / "rootfs.cpio").write_bytes(b"rootfs content\n")
        recipe = self._recipe(packages=(child,))
        alpine_rootfs_state.write_receipt(output, recipe)

        assert alpine_rootfs_state.receipt_matches(output, self._recipe(packages=(child,)))
        assert (recipe) != (self._recipe(packages=(producer,)))
        assert (
            alpine_recipes.alpine_package_recipe(child, "1" * 64, self.signing_key, self.root)
        ) == (
            alpine_recipes.alpine_package_recipe(producer, "1" * 64, self.signing_key, self.root)
        )
        self._write("alpine/aports/not-production/APKBUILD", b"unrelated changed\n")
        assert alpine_rootfs_state.receipt_matches(output, self._recipe(packages=(child,)))
        aport.write_bytes(b"pkgname=fplinux-ncurses\nchanged=yes\n")
        assert not (alpine_rootfs_state.receipt_matches(output, self._recipe(packages=(child,))))

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
        original_recipe = alpine_recipes.alpine_package_recipe

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
            return alpine_aports._build_fplinux_apks(  # noqa: SLF001
                {"triplet": "armv7-alpine-linux-musleabihf"},
                sysroot=Path(self.temporary.name) / "sysroot",
                work=work,
                jobs=1,
                private_key=private_key,
                public_key=public_key,
                build_packages=(*self.packages, child_package),
            )

        with (
            mock.patch.object(alpine_aports, "CACHE", cache),
            mock.patch.object(alpine_packages, "CACHE", cache),
            mock.patch.object(alpine_aports, "ROOT", self.root),
            mock.patch.object(os, "geteuid", return_value=0),
            mock.patch.dict(os.environ, {"FPLINUX_CONTAINER_IMAGE_RECIPE": "1" * 64}),
            # Fake builder processes do not exercise source ownership changes.
            mock.patch.object(
                alpine_aports, "_alpine_source_cache", return_value=nullcontext(sources)
            ),
            mock.patch.object(alpine_aports, "_chown_tree"),
            mock.patch.object(alpine_aports, "_builder_output", side_effect=list_packages),
            mock.patch.object(alpine_aports, "_run_as_builder", side_effect=run_as_builder),
            mock.patch.object(
                alpine_packages,
                "_apk_package_name",
                side_effect=lambda path: path.name.removesuffix("-1.0-r0.apk"),
            ),
            mock.patch.object(alpine_aports, "log_message"),
            mock.patch.object(alpine_aports, "alpine_package_recipe", side_effect=package_recipe),
            mock.patch.object(
                alpine_packages, "alpine_package_recipe", side_effect=package_recipe
            ),
            mock.patch.object(
                alpine_registration, "SUBPACKAGE_APORTS", {child_package: self.packages[0]}
            ),
        ):
            outputs, _public, _private = build_apks()
            assert (builds) == (list(self.packages))
            assert (outputs[child_package].read_bytes()) == (b"fplinux-package-a-extra\n")

            builds.clear()
            outputs, _public, _private = build_apks()
            assert (builds) == ([])
            assert (outputs[child_package].read_bytes()) == (b"fplinux-package-a-extra\n")

            self._write("alpine/aports/not-production/APKBUILD", b"unrelated changed\n")
            build_apks()
            assert (builds) == ([])

            self._write(
                f"alpine/aports/{changed_package}/APKBUILD",
                f"pkgname={changed_package}\npkgrel=1\n".encode(),
            )
            build_apks()
            assert (builds) == ([changed_package])

            slot = cache / alpine_packages.PACKAGE_CACHE_DIRECTORY / changed_package
            receipt = slot / alpine_packages.PACKAGE_RECEIPT_NAME
            package = slot / f"{changed_package}-1.0-r0.apk"
            previous_receipt = receipt.read_bytes()
            previous_package = package.read_bytes()
            self._write(
                f"alpine/aports/{changed_package}/APKBUILD",
                f"pkgname={changed_package}\npkgrel=2\n".encode(),
            )
            failing_package = changed_package
            with pytest.raises(SystemExit, match=f"abuild failed for {changed_package}"):
                build_apks()

        assert (receipt.read_bytes()) == (previous_receipt)
        assert (package.read_bytes()) == (previous_package)

    def test_local_library_apks_populate_fresh_sysroots_on_builds_and_cache_hits(self) -> None:
        """Fake abuild/APK processes exercise dependency files and causal cache decisions."""
        consumer = "fplinux-present"
        library = "fplinux-libdrm"
        unrelated = self.packages[0]
        for name in (consumer, library):
            self._write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
            for source in alpine_selection.shared_aport_sources(name, self.root):
                self._write(source.relative_to(self.root).as_posix(), b"shared source\n")
        library_source = self._write(f"alpine/aports/{library}/library.c", b"library version 1\n")
        cache = Path(self.temporary.name) / "cache"
        sources = cache / "downloads/alpine/sources"
        sources.mkdir(parents=True)
        private_key = self._write("keys/fplinux-build.rsa", b"private key\n")
        public_key = self._write("keys/fplinux-build.rsa.pub", b"public key\n")
        builds: list[str] = []
        original_recipe = alpine_recipes.alpine_package_recipe

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
                    assert ((sysroot / installed).read_bytes()) == (library_source.read_bytes())
            builds.append(cwd.name)
            repository = Path(environment["REPODEST"])
            for filename in list_packages(command, cwd, environment).splitlines():
                (repository / filename).write_bytes(library_source.read_bytes())

        def install_apks(command: list[str]) -> None:
            """Replace APK installation with file markers; signature checks remain untested."""
            assert (command[0]) == ("apk")
            assert ("--no-network") in (command)
            assert ("--no-scripts") in (command)
            assert ("--allow-untrusted") not in (command)
            keys = Path(command[command.index("--keys-dir") + 1])
            assert ((keys / public_key.name).read_bytes()) == (public_key.read_bytes())
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
            alpine_aports._build_fplinux_apks(  # noqa: SLF001
                {"arch": "armv7", "triplet": "armv7-alpine-linux-musleabihf"},
                sysroot=sysroot,
                work=work,
                jobs=1,
                private_key=private_key,
                public_key=public_key,
                build_packages=(consumer, unrelated),
            )
            for installed in (library, f"{library}-dev"):
                assert ((sysroot / installed).read_bytes()) == (library_source.read_bytes())

        with (
            mock.patch.object(alpine_aports, "CACHE", cache),
            mock.patch.object(alpine_packages, "CACHE", cache),
            mock.patch.object(alpine_aports, "ROOT", self.root),
            mock.patch.object(os, "geteuid", return_value=0),
            mock.patch.dict(os.environ, {"FPLINUX_CONTAINER_IMAGE_RECIPE": "1" * 64}),
            # Fake builder processes do not exercise source ownership changes.
            mock.patch.object(
                alpine_aports, "_alpine_source_cache", return_value=nullcontext(sources)
            ),
            mock.patch.object(alpine_aports, "_chown_tree"),
            mock.patch.object(alpine_aports, "_builder_output", side_effect=list_packages),
            mock.patch.object(alpine_aports, "_run_as_builder", side_effect=run_as_builder),
            mock.patch.object(alpine_packages, "run", side_effect=install_apks),
            mock.patch.object(
                alpine_packages,
                "_apk_package_name",
                side_effect=lambda path: path.name.removesuffix("-1.0-r0.apk"),
            ),
            mock.patch.object(alpine_aports, "log_message"),
            mock.patch.object(alpine_aports, "alpine_package_recipe", side_effect=package_recipe),
            mock.patch.object(
                alpine_packages, "alpine_package_recipe", side_effect=package_recipe
            ),
        ):
            build_apks()
            assert (set(builds)) == ({library, consumer, unrelated})
            builds.clear()
            build_apks()
            assert (builds) == ([])
            self._write(f"alpine/aports/{consumer}/program.c", b"changed consumer\n")
            build_apks()
            assert (builds) == ([consumer])
            builds.clear()
            library_source.write_bytes(b"library version 2\n")
            build_apks()
            assert (set(builds)) == ({library, consumer})

    @pytest.mark.parametrize(
        ("cached_bundle", "solver_status"),
        [
            pytest.param(False, 0, id="bundle-miss-installable"),
            pytest.param(False, 7, id="bundle-miss-unresolved"),
            pytest.param(True, 0, id="bundle-hit-installable"),
            pytest.param(True, 7, id="bundle-hit-unresolved"),
        ],
    )
    def test_cached_rootfs_reuse_honors_mocked_bundle_solver_results(
        self, *, cached_bundle: bool, solver_status: int
    ) -> None:
        """Stub APK builds and CPIO/solver processes; reuse must honor installation errors."""
        cache = Path(self.temporary.name) / "cache"
        output = cache / "rootfs" / ("9" * 64)
        output.mkdir(parents=True)
        rootfs = output / alpine_rootfs_state.ROOTFS_NAME
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
            mock.patch.object(alpine_rootfs, "CACHE", cache),
            mock.patch.dict(os.environ, {"FPLINUX_CONTAINER_IMAGE_RECIPE": "1" * 64}),
            mock.patch.object(
                alpine_rootfs,
                "_ensure_apk_signing_key",
                return_value=(private_key, public_key, "2" * 64),
            ),
            mock.patch.object(alpine_rootfs, "alpine_rootfs_recipe", return_value="9" * 64),
            mock.patch.object(alpine_rootfs_state, "receipt_matches", return_value=True),
            mock.patch.object(
                alpine_rootfs,
                "_cached_aport_packages",
                side_effect=({rootfs_package: base_apk}, cached_outputs),
            ),
            mock.patch.object(alpine_lock, "load_alpine_lock", return_value=lock),
            mock.patch.object(alpine_lock, "package_records", return_value={}),
            mock.patch.object(sources_build, "fetch", return_value=archive),
            mock.patch.object(alpine_rootfs, "_alpine_runtime_packages", return_value=[]),
            mock.patch.object(alpine_rootfs, "_alpine_group_packages", return_value=[]),
            mock.patch.object(tarfile, "open", return_value=archive_context),
            mock.patch.object(alpine_rootfs, "_prepare_alpine_sysroot"),
            mock.patch.object(process_build, "log_message"),
            mock.patch.object(
                alpine_rootfs,
                "_build_fplinux_apks",
                return_value=(
                    {rootfs_package: base_apk, bundle_package: bundle_apk},
                    private_key,
                    public_key,
                ),
            ),
            mock.patch.object(
                alpine_rootfs,
                "_build_alpine_composition_repository",
                side_effect=AssertionError("rootfs cache hit must not recompose the rootfs"),
            ),
            mock.patch.object(subprocess, "run", side_effect=run_external),
        ):
            if solver_status:
                with pytest.raises(SystemExit, match="offline: missing runtime dependency"):
                    alpine_rootfs.build_rootfs(2, (rootfs_package,), (bundle_package,))
            else:
                actual = alpine_rootfs.build_rootfs(2, (rootfs_package,), (bundle_package,))
                assert (actual[:3]) == ((rootfs, output, "9" * 64))
                assert (actual[3]) == ({bundle_package: bundle_apk})
            assert (rootfs.read_bytes()) == (b"rootfs\n")

    def test_mocked_rootfs_cache_hits_reuse_cached_outputs(self) -> None:
        """A mocked receipt hit returns cached rootfs paths without bundle outputs."""
        cache = Path(self.temporary.name) / "cache"
        package = self.packages[0]
        recipes = ("3" * 64, "4" * 64)
        cached_package = self._write("cached/package.apk", b"package\n")
        for recipe in recipes:
            output = cache / "rootfs" / recipe
            output.mkdir(parents=True)
            rootfs = output / alpine_rootfs_state.ROOTFS_NAME
            rootfs.write_bytes(b"rootfs\n")
            with (
                mock.patch.object(alpine_rootfs, "CACHE", cache),
                mock.patch.object(
                    alpine_rootfs,
                    "_ensure_apk_signing_key",
                    return_value=(Path("private"), Path("public"), "5" * 64),
                ),
                mock.patch.object(alpine_rootfs, "alpine_rootfs_recipe", return_value=recipe),
                mock.patch.object(alpine_rootfs_state, "receipt_matches", return_value=True),
                mock.patch.object(
                    alpine_rootfs,
                    "_cached_aport_packages",
                    return_value={package: cached_package},
                ),
            ):
                actual_rootfs, actual_output, actual_recipe, bundle_outputs = (
                    alpine_rootfs.build_rootfs(1, (package,))
                )
            assert ((actual_rootfs, actual_output, actual_recipe)) == ((rootfs, output, recipe))
            assert (bundle_outputs) == ({})

    def test_signing_key_identity_requires_one_regular_public_key(self) -> None:
        """Keep package signing state explicit rather than silently generating it in prune."""
        cache = Path(self.temporary.name) / "cache"
        key = alpine_signing.signing_public_key(cache)
        with pytest.raises(SystemExit, match="signing public key"):
            alpine_signing.signing_key_identity(cache)
        key.parent.mkdir(parents=True)
        key.write_bytes(b"public-key\n")
        assert (alpine_signing.signing_key_identity(cache)) == (
            hashlib.sha256(key.read_bytes()).hexdigest()
        )

    def test_bootstrap_change_invalidates_ram_receipt_but_not_external_root(self) -> None:
        """RAM reuse follows bootstrap and boot archive bytes; external roots stay reusable."""
        ram_recipe = self._recipe()
        external_recipe = self._recipe(root_kind="external")
        cache = Path(self.temporary.name) / "cache"
        ram_output = alpine_rootfs_state.rootfs_output(cache, ram_recipe)
        external_output = alpine_rootfs_state.rootfs_output(cache, external_recipe)
        for output in (ram_output, external_output):
            output.mkdir(parents=True)
            (output / "rootfs.cpio").write_bytes(b"logical composition\n")
        initramfs = ram_output / "initramfs.cpio"
        initramfs.write_bytes(b"boot archive\n")
        alpine_rootfs_state.write_receipt(ram_output, ram_recipe)
        alpine_rootfs_state.write_receipt(external_output, external_recipe)
        assert alpine_rootfs_state.receipt_matches(ram_output, self._recipe())
        assert alpine_rootfs_state.receipt_matches(
            external_output, self._recipe(root_kind="external")
        )
        initramfs.write_bytes(b"boot archivf\n")
        assert not (alpine_rootfs_state.receipt_matches(ram_output, ram_recipe))
        initramfs.write_bytes(b"boot archive\n")
        assert alpine_rootfs_state.receipt_matches(ram_output, ram_recipe)
        self._write("unrelated.txt", b"unrelated edit\n")
        assert alpine_rootfs_state.receipt_matches(ram_output, self._recipe())
        self.bootstrap.write_bytes(b"#!/bin/sh\nexit 1\n")
        changed = self._recipe()
        assert not (alpine_rootfs_state.receipt_matches(ram_output, changed))
        assert alpine_rootfs_state.receipt_matches(
            external_output, self._recipe(root_kind="external")
        )
        rebuilt = alpine_rootfs_state.rootfs_output(cache, changed)
        rebuilt.mkdir(parents=True)
        (rebuilt / "rootfs.cpio").write_bytes(b"logical composition\n")
        (rebuilt / "initramfs.cpio").write_bytes(b"changed boot archive\n")
        alpine_rootfs_state.write_receipt(rebuilt, changed)
        assert alpine_rootfs_state.receipt_matches(rebuilt, changed)
        assert (alpine_rootfs_state.trusted_receipt_identity(ram_output, ram_recipe)) != (
            alpine_rootfs_state.trusted_receipt_identity(rebuilt, changed)
        )

    def test_receipt_matches_only_exact_rootfs_bytes(self) -> None:
        """A successful receipt is revoked by any rootfs byte change."""
        recipe = self._recipe()
        cache = Path(self.temporary.name) / "cache"
        output = alpine_rootfs_state.rootfs_output(cache, recipe)
        output.mkdir(parents=True)
        rootfs = output / alpine_rootfs_state.ROOTFS_NAME
        rootfs.write_bytes(b"rootfs\n")
        (output / "initramfs.cpio").write_bytes(b"boot archive\n")
        alpine_rootfs_state.write_receipt(output, recipe)

        assert alpine_rootfs_state.receipt_matches(output, recipe)
        identity = alpine_rootfs_state.trusted_receipt_identity(output, recipe)
        assert (identity["recipe"]) == (recipe)

        rootfs.write_bytes(b"tampered\n")
        assert not (alpine_rootfs_state.receipt_matches(output, recipe))

    def test_receipt_with_unknown_shape_is_a_cache_miss(self) -> None:
        """An unrecognized cache artifact is ignored without migration."""
        recipe = self._recipe()
        output = Path(self.temporary.name) / "output"
        output.mkdir()
        (output / alpine_rootfs_state.ROOTFS_NAME).write_bytes(b"rootfs\n")
        (output / "initramfs.cpio").write_bytes(b"boot archive\n")
        alpine_rootfs_state.write_receipt(output, recipe)
        receipt_path = output / alpine_rootfs_state.RECEIPT_NAME
        receipt = json.loads(receipt_path.read_text())
        receipt["unknown"] = "receipt-field"
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")

        assert not (alpine_rootfs_state.receipt_matches(output, recipe))
