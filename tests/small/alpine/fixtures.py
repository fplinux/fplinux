# SPDX-License-Identifier: GPL-2.0-only
"""Temporary source inputs for Alpine tests."""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.alpine import recipes as alpine_recipes

from tests import ROOT

ALPINE_FILES = ROOT / "tests/fixtures/alpine"

if TYPE_CHECKING:
    from collections.abc import Iterator


class AlpineSourceContext:
    """Prepare temporary Alpine recipe inputs without defining scenarios."""

    def __init__(self, directory: Path) -> None:
        """Prepare source inputs below the case's owned temporary directory."""
        self.directory = directory
        self.root = directory / "source"
        self.root.mkdir()
        self.signing_key = "d" * 64
        self.packages = ("fplinux-package-a", "fplinux-package-b")
        self.write("alpine.lock.toml", (ALPINE_FILES / "alpine.lock.toml").read_bytes())
        self.write("sources.lock.toml", (ALPINE_FILES / "sources.lock.toml").read_bytes())
        self.write("alpine/abuild.conf", b"PACKAGER=FPLinux\n")
        for name in self.packages:
            self.write(f"alpine/aports/{name}/APKBUILD", f"pkgname={name}\n".encode())
        self.aport = self.root / "alpine/aports/fplinux-package-a/APKBUILD"
        self.write("alpine/aports/not-production/APKBUILD", b"pkgname=not-production\n")
        for source in (
            "lock.py",
            "selection.py",
            "recipes.py",
            "sources.py",
            "signing.py",
            "aports.py",
            "packages.py",
            "rootfs.py",
            "rootfs_state.py",
            "rootfs_verify.py",
            "rootfs_files.py",
        ):
            self.write(f"scripts/fplinux_cli/alpine/{source}", b"Alpine implementation\n")
        self.write("scripts/fplinux_cli/build/kernel/compile.py", b"builder implementation\n")
        self.write("scripts/fplinux_cli/build/inputs.py", b"build input validation\n")
        self.write("scripts/fplinux_cli/build/process.py", b"build process execution\n")
        self.write("scripts/fplinux_cli/build/sources.py", b"source fetching\n")
        for owner in (
            "environment/downloads.py",
            "manifests/values.py",
            "manifests/source_catalog.py",
        ):
            relative = f"scripts/fplinux_cli/{owner}"
            self.write(relative, (ROOT / relative).read_bytes())
        self.write("scripts/fplinux_cli/common.py", b"shared archive and file operations\n")
        self.write("scripts/fplinux_cli/build/environment.py", b"build environment\n")
        self.write("scripts/fplinux_cli/device_data/inputs.py", b"firmware inputs\n")
        self.shared_source = self.write("alpine/shared/shared.c", b"int shared;\n")
        self.bootstrap = self.write("alpine/ramroot-init.sh", b"#!/bin/sh\nexit 0\n")

    def write(self, relative: str, contents: bytes) -> Path:
        """Write one fixture file below the temporary source root."""
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def recipe(  # noqa: PLR0913 -- keep independently varied rootfs inputs explicit.
        self,
        image: str = "1" * 64,
        signing_key: str | None = None,
        packages: tuple[str, ...] | None = None,
        display_brightness: dict[str, Any] | None = None,
        *,
        root_kind: str = "initramfs",
    ) -> str:
        """Compute the rootfs recipe for this checkout and the scenario's selected inputs."""
        return alpine_recipes.alpine_rootfs_recipe(
            image,
            self.signing_key if signing_key is None else signing_key,
            self.packages if packages is None else packages,
            self.root,
            display_brightness=display_brightness,
            root_kind=root_kind,
        )


@pytest.fixture
def alpine_source(tmp_path: Path) -> Iterator[AlpineSourceContext]:
    """Release all mutable Alpine trees as soon as the scenario finishes."""
    with tempfile.TemporaryDirectory(dir=tmp_path) as temporary:
        yield AlpineSourceContext(Path(temporary))
