# SPDX-License-Identifier: GPL-2.0-only
"""Temporary source inputs for Alpine tests."""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.alpine import recipes as alpine_recipes

if TYPE_CHECKING:
    from collections.abc import Iterator


class AlpineSourceFixture:
    """Prepare temporary Alpine recipe inputs without defining scenarios."""

    @pytest.fixture(autouse=True)
    def alpine_source(self) -> Iterator[None]:
        """Create one complete minimal Alpine rootfs recipe fixture."""
        self.temporary = tempfile.TemporaryDirectory()
        with self.temporary:
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
            for source in (
                "lock.py",
                "selection.py",
                "recipes.py",
                "signing.py",
                "aports.py",
                "packages.py",
                "rootfs.py",
                "rootfs_state.py",
                "rootfs_verify.py",
                "rootfs_files.py",
            ):
                self._write(f"scripts/fplinux_cli/alpine/{source}", b"Alpine implementation\n")
            self._write("scripts/fplinux_cli/build/kernel/compile.py", b"builder implementation\n")
            self._write("scripts/fplinux_cli/build/inputs.py", b"build input validation\n")
            self._write("scripts/fplinux_cli/build/process.py", b"build process execution\n")
            self._write("scripts/fplinux_cli/build/sources.py", b"source fetching\n")
            self._write("scripts/fplinux_cli/common.py", b"shared archive and file operations\n")
            self._write("scripts/fplinux_cli/build/environment.py", b"build environment\n")
            self._write("scripts/fplinux_cli/device_data/inputs.py", b"firmware inputs\n")
            self.shared_source = self._write("alpine/shared/shared.c", b"int shared;\n")
            self.bootstrap = self._write("alpine/ramroot-init.sh", b"#!/bin/sh\nexit 0\n")
            yield

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
        return alpine_recipes.alpine_rootfs_recipe(
            image,
            self.signing_key if signing_key is None else signing_key,
            self.packages if packages is None else packages,
            self.root,
            display_brightness=display_brightness,
            root_kind=root_kind,
        )
