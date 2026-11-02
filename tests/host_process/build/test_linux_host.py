# SPDX-License-Identifier: GPL-2.0-only
"""Run the relocated Linux SSH launcher with a controlled runtime."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

from fplinux_cli.build.host_tools.linux_host import _write_launchers

if TYPE_CHECKING:
    from pathlib import Path


def test_ssh_launcher_selects_the_relocated_runtime_with_literal_arguments(tmp_path: Path) -> None:
    """Replace the external loader with an argument recorder, without connecting to a peer."""
    package = tmp_path / "extracted archive"
    _write_launchers(package)
    runtime = package / "runtime"
    library = runtime / "lib"
    library.mkdir(parents=True)
    loader = library / "ld-musl-x86_64.so.1"
    loader.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n', encoding="utf-8")
    loader.chmod(0o755)
    result = subprocess.run(
        [str(package / "tools/linux/ssh"), "-V"],
        cwd=tmp_path,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
        capture_output=True,
        text=True,
        check=True,
    )
    assert result.stdout.splitlines() == [
        "--library-path",
        str(library),
        str(runtime / "bin/ssh"),
        "-V",
    ]
