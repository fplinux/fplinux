# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for read-only system identity."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_system_identity_reads_release_fields_and_model(tmp_path: Path) -> None:
    executable = tmp_path / "system-info"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            str(ROOT / "tests/host_tool/gui/settings/system-info.c"),
            str(ROOT / "lib/fplinux-shared/fplinux-system-info.c"),
        ],
        name="compile system identity harness",
    )
    release = tmp_path / "os-release"
    release.write_text('NAME="FPLinux"\nVERSION_ID=1.0.0\nBUILD_ID="012345abcdef"\n')
    version = tmp_path / "version"
    version.write_text("NAME=FPLinux\nVERSION_ID=1\nVERSION='Rolling'\n")
    missing = tmp_path / "missing"
    model = tmp_path / "model"
    model.write_bytes(b"Phone model\0ignored")
    result = run_process(
        [str(executable), str(release), str(version), str(missing), str(model)],
        name="run system identity harness",
        timeout=10,
        check=True,
    )
    assert result.stdout == "system-info: all checks passed\n"
