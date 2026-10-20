# SPDX-License-Identifier: GPL-2.0-only
"""Host component test for the shared C11 numeric-keypad multi-tap core."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

CORE = ROOT / "lib/fplinux/fplinux-multitap.c"
LANGUAGE = ROOT / "lib/fplinux/fplinux-language.c"
HARNESS = ROOT / "tests/host_tool/input/fplinux-multitap-core.c"
INCLUDE = ROOT / "include/fplinux"
LANGUAGES = ROOT / "tests/host_tool/input/fixtures/input-languages"


def language_file(base: bytes, **overrides: bytes) -> bytes:
    """Return the English fixture with whole key=value lines replaced."""
    lines = []
    for line in base.splitlines():
        key = line.split(b"=", 1)[0].decode()
        lines.append(overrides.pop(key.replace(".", "_"), line))
    lines.extend(overrides.values())
    return b"\n".join(lines) + b"\n"


@pytest.fixture
def languages(tmp_path: Path) -> Path:
    """Install en and ru plus one valid and four excluded test languages."""
    for code in ("en", "ru"):
        shutil.copy(LANGUAGES / code, tmp_path / code)
    english = (LANGUAGES / "en").read_bytes()
    files = {
        "xx": language_file(
            english,
            name=b"name=Test",
            multitap_2="multitap.2=ß€😀2".encode(),
            multitap_3=b"multitap.3=3ab",
        ),
        "sy": language_file(english, name=b"name=System only", input=b"input=no"),
        "ww": language_file(english, input_indicator_upper=b"input.indicator.upper="),
        "yy": language_file(english, multitap_4=b"multitap.4=gh\xffi4"),
        "zz": language_file(english, multitap_5=b"multitap.5=jkl"),
    }
    for code, contents in files.items():
        (tmp_path / code).write_bytes(contents)
    return tmp_path


class MultiTapCoreTests:
    """Exercise the portable state machine without target hardware."""

    @pytest.fixture(scope="class")
    @classmethod
    def executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "multitap-core") as temporary:
            executable = temporary / "fplinux-multitap-core"
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(INCLUDE),
                    str(HARNESS),
                    str(CORE),
                    str(LANGUAGE),
                ],
                name="compile multi-tap harness",
                check=True,
            )
            yield executable

    def test_c11_core_contract(self, executable: Path, languages: Path) -> None:
        """UTF-8 language maps, held digits and composition boundaries hold."""
        run_process(
            [str(executable), str(languages)],
            name="run multi-tap harness",
            timeout=10,
            check=True,
        )
