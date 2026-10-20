# SPDX-License-Identifier: GPL-2.0-only
"""Host filesystem contracts for installed-content fingerprints."""

from __future__ import annotations

import os
import shutil
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.environment.image_content import image_content_digest, image_content_records

from tests.small.artifacts.image_fixtures import installed_tree

if TYPE_CHECKING:
    from pathlib import Path


class ImageContentTests:
    """Installed file changes are causal; operational path and time are not."""

    @staticmethod
    def test_actual_records_describe_files_links_owners_and_modes(tmp_path: Path) -> None:
        """The digest input preserves actual ownership, set-id modes and link targets."""
        root = tmp_path
        compiler = root / "usr/bin/cc"
        compiler.parent.mkdir(parents=True)
        (root / "usr").chmod(0o751)
        compiler.parent.chmod(0o2750)
        compiler.write_bytes(b"compiler\n")
        compiler.chmod(0o4755)
        (compiler.parent / "compiler").symlink_to("cc")
        (root / "etc").mkdir(mode=0o755)
        (root / "etc").chmod(0o755)
        owner = {"uid": os.getuid(), "gid": os.getgid()}
        assert image_content_records(root) == [
            {"path": "etc", "mode": 0o755, "type": "directory", **owner},
            {"path": "usr", "mode": 0o751, "type": "directory", **owner},
            {"path": "usr/bin", "mode": 0o2750, "type": "directory", **owner},
            {
                "path": "usr/bin/cc",
                "mode": 0o4755,
                "type": "file",
                "sha256": "850d9371d0ce2cb083d4075d7eec49f2f6f3c2bee5b58d0226b36028ae84efe0",
                **owner,
            },
            {
                "path": "usr/bin/compiler",
                "mode": 0o777,
                "type": "symlink",
                "target": "cc",
                **owner,
            },
        ]

    @staticmethod
    def test_same_installed_content_ignores_location_time_and_generation(tmp_path: Path) -> None:
        """Two installations have one identity despite their invocation-specific state."""
        first = tmp_path / "first"
        second = tmp_path / "other-location"
        installed_tree(first)
        shutil.copytree(first, second, symlinks=True)
        (second / "etc/fplinux-environment-state").write_bytes(b"new operational generation\n")
        for relative in ("etc/hostname", "etc/hosts", "etc/resolv.conf"):
            (second / relative).write_bytes(b"runtime host data\n")
        os.utime(second / "usr/bin/cc", (1000000000, 1000000000))
        assert (image_content_digest(first)) == (image_content_digest(second))

    @staticmethod
    @pytest.mark.parametrize(
        "relative", ["usr/bin/cc", "usr/lib/libc.so"], ids=["compiler", "library"]
    )
    def test_actual_compiler_and_library_bytes_change_identity(
        tmp_path: Path, relative: str
    ) -> None:
        """A replaced installed compiler or linked library invalidates artifact recipes."""
        root = tmp_path
        installed_tree(root)
        original = image_content_digest(root)
        (root / relative).write_bytes(b"changed effective input\n")
        assert (image_content_digest(root)) != (original)

    @staticmethod
    def test_tool_mode_and_symlink_resolution_change_identity(tmp_path: Path) -> None:
        """Executable permissions and the selected tool target remain effective inputs."""
        root = tmp_path
        compiler = installed_tree(root)
        original = image_content_digest(root)
        compiler.chmod(0o644)
        assert (image_content_digest(root)) != (original)
        compiler.chmod(0o755)
        alias = root / "usr/bin/compiler"
        alias.unlink()
        alias.symlink_to("other-compiler")
        assert (image_content_digest(root)) != (original)

    @staticmethod
    def test_installed_configuration_is_causal_but_scratch_is_not(tmp_path: Path) -> None:
        """Only files supplied by the installed environment enter its tool identity."""
        root = tmp_path
        installed_tree(root)
        original = image_content_digest(root)
        scratch = root / "tmp"
        scratch.mkdir()
        (scratch / "log").write_bytes(b"setup output\n")
        assert (image_content_digest(root)) == (original)
        (root / "etc/compiler.conf").write_bytes(b"effective flags\n")
        assert (image_content_digest(root)) != (original)
