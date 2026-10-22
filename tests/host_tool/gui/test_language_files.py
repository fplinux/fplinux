# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for language definitions and the shared reader."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_language_reader_loads_shipped_files_and_rejects_invalid_definitions(
    tmp_path: Path,
) -> None:
    executable = tmp_path / "fplinux-language"
    shipped = tmp_path / "shipped"
    scratch = tmp_path / "scratch"
    shipped.mkdir()
    scratch.mkdir()
    for code in ("en", "ru"):
        shutil.copyfile(ROOT / "alpine/aports/fplinux-language" / code, shipped / code)
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
        ],
        name="compile language reader harness",
    )
    result = run_process(
        [str(executable), str(shipped), str(scratch)],
        name="read language definitions and exercise validation",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-language: OK\n"
