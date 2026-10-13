# SPDX-License-Identifier: GPL-2.0-only
"""Temporary Linux archives and board manifests for source preparation tests."""

from __future__ import annotations

import copy
import shutil
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.kernel import prepare as linux_build
from fplinux_cli.manifests.linux import discover_linux_targets

if TYPE_CHECKING:
    from collections.abc import Iterator

    from fplinux_cli.build.kernel import state as linux_state

SOURCE_FILES = Path(__file__).with_name("linux-source")


@dataclass
class LinuxSourceContext:
    """Own one archive, source checkout and isolated mutable board declarations."""

    root: Path
    cache: Path
    archive: Path
    sources: dict[str, Any]
    fetch: mock.Mock

    def platform(self, name: str, *, source_lock: str = "linux") -> None:
        """Write the Linux-only platform manifest consumed by real discovery."""
        directory = self.root / "platforms" / name
        directory.mkdir(parents=True)
        (directory / "linux/overlay").mkdir(parents=True)
        template = (SOURCE_FILES / "platform.toml.in").read_text(encoding="utf-8")
        (directory / "platform.toml").write_text(
            template % {"name": name, "name_upper": name.upper(), "source_lock": source_lock},
            encoding="utf-8",
        )

    def target(self, name: str, *, platform: str = "soc", extra: str = "") -> None:
        """Add a board with one independent driver and one guarded Kconfig fragment."""
        directory = self.root / "targets" / name
        directory.mkdir(parents=True)
        (directory / "linux/overlay").mkdir(parents=True)
        (directory / "driver.c").write_text(f"int {name} = 1;\n", encoding="utf-8")
        (directory / "fragment").write_text(
            f'config {name.upper()}\n\tbool "{name}"\n', encoding="utf-8"
        )
        template = (SOURCE_FILES / "target.toml.in").read_text(encoding="utf-8")
        (directory / "target.toml").write_text(
            template % {"name": name, "platform": platform} + extra,
            encoding="utf-8",
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
        """Observe fixture file contents, source inode and mtime."""
        return {
            path.relative_to(source).as_posix(): (
                path.read_bytes(),
                path.stat().st_ino,
                path.stat().st_mtime_ns,
            )
            for path in source.rglob("*")
            if path.is_file()
        }


@pytest.fixture
def linux_source(tmp_path: Path) -> Iterator[LinuxSourceContext]:
    """Prepare two boards; replace remote archive fetching with a local archive."""
    with tempfile.TemporaryDirectory(dir=tmp_path) as temporary:
        root = Path(temporary)
        archive_root = root / "upstream/linux-test"
        for fixture in (SOURCE_FILES / "upstream").rglob("*.fixture"):
            relative = fixture.relative_to(SOURCE_FILES / "upstream").as_posix()
            destination = archive_root / relative.removesuffix(".fixture")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(fixture, destination)
        archive = root / "linux-test.tar.xz"
        with tarfile.open(archive, "w:xz") as output:
            output.add(archive_root, arcname="linux-test")
        sources = {
            "linux": {
                "version": "test",
                "sha256": common.sha256_file(archive),
                "url": "https://example.invalid/linux-test.tar.xz",
            }
        }
        cache = root / "cache"
        with (
            mock.patch.object(common, "ROOT", root),
            mock.patch.object(inputs_build, "CACHE", cache),
            mock.patch.object(sources_build, "fetch", return_value=archive) as fetch,
        ):
            context = LinuxSourceContext(root, cache, archive, sources, fetch)
            context.platform("soc")
            context.target("alpha")
            context.target("beta")
            yield context
