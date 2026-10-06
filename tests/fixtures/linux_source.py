# SPDX-License-Identifier: GPL-2.0-only
"""Temporary Linux source: a local upstream xz archive, one platform and two board manifests."""

from __future__ import annotations

import copy
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

from fplinux_cli import common
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.kernel import prepare as linux_build
from fplinux_cli.manifests.linux import discover_linux_targets

if TYPE_CHECKING:
    from fplinux_cli.build.kernel import state as linux_state


class LinuxSourceFixture(unittest.TestCase):
    """Provide a two-board Linux source and the production preparer; define no test cases."""

    def setUp(self) -> None:
        """Create a two-board source fixture; only remote archive fetching is replaced."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / "cache"
        archive_root = self.root / "upstream/linux-test"
        (archive_root / "drivers").mkdir(parents=True)
        for name, contents in {
            "Makefile": "VERSION = test\n",
            ".clang-format": "BasedOnStyle: LLVM\n",
            "README": "untouched upstream\n",
            "drivers/Kconfig": 'menu "Drivers"\nendmenu\n',
            "drivers/common.c": "int common = 1;\n",
            "drivers/replaced.c": "upstream original\n",
        }.items():
            (archive_root / name).write_text(contents)
        self.archive = self.root / "linux-test.tar.xz"
        with tarfile.open(self.archive, "w:xz") as output:
            output.add(archive_root, arcname="linux-test")
        self.sources: dict[str, Any] = {
            "linux": {
                "version": "test",
                "sha256": common.sha256_file(self.archive),
                "url": "https://example.invalid/linux-test.tar.xz",
            }
        }
        self.platform("soc")
        self.target("alpha")
        self.target("beta")
        self.enterContext(mock.patch.object(common, "ROOT", self.root))
        self.enterContext(mock.patch.object(inputs_build, "CACHE", self.cache))
        self.fetch = self.enterContext(
            mock.patch.object(sources_build, "fetch", return_value=self.archive)
        )

    def platform(self, name: str, *, source_lock: str = "linux") -> None:
        """Write the Linux-only platform manifest consumed by real discovery."""
        directory = self.root / "platforms" / name
        directory.mkdir(parents=True)
        (directory / "platform.toml").write_text(
            f"""[identity]
vendor = "Example"
soc = "{name.upper()}"
aliases = []
compatible = "example,{name}"
[linux]
arch = "arm"
source_lock = "{source_lock}"
dts_directory = "arch/{name}/boot/dts"
platform_identity_header = "include/{name}-identity.h"
patches = []
copies = []
appends = []
"""
        )

    def target(self, name: str, *, platform: str = "soc", extra: str = "") -> None:
        """Add a board with one independent driver and one guarded Kconfig fragment."""
        directory = self.root / "targets" / name
        directory.mkdir(parents=True)
        (directory / "driver.c").write_text(f"int {name} = 1;\n")
        (directory / "fragment").write_text(f'config {name.upper()}\n\tbool "{name}"\n')
        (directory / "target.toml").write_text(
            f"""platform = "{platform}"
[identity]
brand = "Example"
product = "{name}"
hardware_codes = []
compatible = "example,{name}"
[linux]
patches = []
[[linux.copies]]
source = "driver.c"
destination = "drivers/{name}.c"
[[linux.appends]]
source = "fragment"
destination = "drivers/Kconfig"
{extra}"""
        )

    def prepare(
        self, name: str, *, external: bool = False
    ) -> tuple[Path, linux_state.PreparedLinuxState]:
        """Load a selected Linux context and run the production preparer."""
        digest = self.sources["linux"]["sha256"]
        selected = next(
            target
            for target in discover_linux_targets(self.root, self.sources, digest)
            if target.name == name
        )
        config = copy.deepcopy(selected.config)
        config["linux"]["root"] = (
            {
                "kind": "external",
                "partuuid": "12345678-02",
                "filesystem": "ext4",
                "wait_seconds": 5,
            }
            if external
            else {"kind": "initramfs"}
        )
        config["profile"] = "sd" if external else "default"
        return linux_build.prepare_linux(self.sources, name, config, selected.platform)

    @staticmethod
    def snapshot(source: Path) -> dict[str, tuple[bytes, int, int]]:
        """Observe all files in this small fixture, including source inode and mtime."""
        return {
            path.relative_to(source).as_posix(): (
                path.read_bytes(),
                path.stat().st_ino,
                path.stat().st_mtime_ns,
            )
            for path in source.rglob("*")
            if path.is_file()
        }
