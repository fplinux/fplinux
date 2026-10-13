# SPDX-License-Identifier: GPL-2.0-only
"""Owned source files shared by immutable workspace scenarios."""

from __future__ import annotations

from contextlib import contextmanager
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli.workspace import build_inputs, quality_inputs, staging

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


class WorkspaceSourceFixture:
    """Create an owned source file with explicit contents and mode."""

    @staticmethod
    def _source(root: Path, *, contents: bytes = b"source", mode: int = 0o754) -> Path:
        source = root / "source"
        source.write_bytes(contents)
        source.chmod(mode)
        return source

    @staticmethod
    def _registration_source(root: Path) -> tuple[str, Path]:
        """Create the package declarations materialized beside a synthetic target."""
        relative = "scripts/fplinux_cli/alpine/registration.py"
        source = root / relative
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(b"# empty package declarations\n")
        return relative, source

    @staticmethod
    def _source_lock_files(root: Path, target: str = "demo") -> list[tuple[str, Path]]:
        """Supply captured lock inputs beside controlled target and package discovery."""
        contents = {
            "sources.lock.toml": (
                f'[linux]\nsha256 = "{"1" * 64}"\n'
                '[bootstrap]\narchive_url = "https://example.invalid/boot.tar.gz"\n'
                f'archive_sha256 = "{"2" * 64}"\n'
                '[host]\narchive_url = "https://example.invalid/host.tar.gz"\n'
                f'archive_sha256 = "{"3" * 64}"\n'
                '[unused]\nurl = "https://example.invalid/unused.tar.gz"\n'
                f'sha256 = "{"4" * 64}"\n'
            ),
            "platforms/demo/platform.toml": (
                '[linux]\nsource_lock = "linux"\n'
                '[bootstrap]\nvendor_source_lock = "bootstrap"\n'
                '[[host.tools]]\ntype = "make-archive"\nsource_lock = "host"\n'
            ),
            f"targets/{target}/target.toml": 'platform = "demo"\n',
            "platforms/demo/loader/assets.lock.toml": (
                '[fdl1]\nurl = "https://example.invalid/fdl1.bin"\n'
                f'sha256 = "{"5" * 64}"\ncache_name = "fdl1.bin"\n'
                'license = "MIT"\npath = "fdl1.bin"\n'
                '[maps]\nurl = "https://example.invalid/maps.7z"\n'
                f'sha256 = "{"6" * 64}"\ncache_name = "maps.7z"\n'
                'license = "MIT"\npinmap_path = "pinmap.bin"\nkeymap_path = "keymap.bin"\n'
            ),
        }
        result = []
        for relative, text in contents.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
            result.append((relative, path))
        return result


@contextmanager
def workspace_root(root: Path) -> Iterator[None]:
    """Bind snapshot selection and materialization to one owned temporary checkout."""
    with (
        mock.patch.object(build_inputs, "ROOT", root),
        mock.patch.object(quality_inputs, "ROOT", root),
        mock.patch.object(staging, "ROOT", root),
    ):
        yield


@contextmanager
def empty_package_graph() -> Iterator[None]:
    """Keep package selection empty when a snapshot fixture supplies only source inputs."""
    with (
        mock.patch.object(build_inputs, "selected_packages", return_value=()),
        mock.patch.object(build_inputs, "bundle_packages", return_value=()),
        mock.patch.object(build_inputs, "selected_aport_graph", return_value=()),
    ):
        yield
