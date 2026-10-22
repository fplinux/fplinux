# SPDX-License-Identifier: GPL-2.0-only
"""Bounded Unicode scalar decoding through the shared C reader."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def utf8_reader(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link only the scalar reader and its literal-input harness."""
    with c_build_directory(tmp_path_factory, "utf8") as directory:
        executable = directory / "utf8"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include/fplinux"),
                str(ROOT / "tests/host_tool/input/fplinux-utf8.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            ],
            name="compile bounded UTF-8 reader harness",
        )
        yield executable


@pytest.mark.parametrize("locale", ["C", "C.UTF-8"])
def test_cursor_advances_only_for_valid_bounded_scalars(utf8_reader: Path, locale: str) -> None:
    """Scalars include NUL; empty and malformed input preserve both outputs in every locale."""
    result = run_process(
        [str(utf8_reader)],
        name="check bounded UTF-8 cursor and scalar results",
        env={"LC_ALL": locale},
        timeout=10,
        check=True,
    )
    assert result.stdout == ""
    assert result.stderr == ""
