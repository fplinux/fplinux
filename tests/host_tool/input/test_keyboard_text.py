# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for keyboard layout selection.

The C driver links the shared layout and text-filter source. It does not
compile a keymap with xkbcommon or open an input device.
"""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SHARED_INCLUDE = ROOT / "include/fplinux"
SOURCE = ROOT / "lib/fplinux/fplinux-keyboard-text.c"
HARNESS = ROOT / "tests/host_tool/input/fplinux-keyboard-text.c"


class KeyboardTextTests:
    """Compose layouts from temporary data roots with literal layout files."""

    @pytest.fixture(scope="class")
    @classmethod
    def executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
        """Compile the host driver once for this test class."""
        with c_build_directory(tmp_path_factory, "keyboard-text") as temporary:
            executable = temporary / "fplinux-keyboard-text"
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(SHARED_INCLUDE),
                    str(HARNESS),
                    str(SOURCE),
                ],
                name="compile shared keyboard text driver",
                check=True,
            )
            yield executable

    def data_root(self, work: Path, layouts: tuple[str, ...] | None) -> Path:
        """Create a data root whose layouts directory holds empty named files."""
        root = Path(tempfile.mkdtemp(dir=work))
        if layouts is not None:
            (root / "layouts").mkdir()
            for layout in layouts:
                (root / "layouts" / layout).write_bytes(b"")
        return root

    def compose(
        self, executable: Path, work: Path, mode: str, layouts: tuple[str, ...] | None
    ) -> tuple[int, str, str]:
        """Run the driver for one data root and return status, stdout and stderr."""
        result = run_process(
            [str(executable), mode, str(self.data_root(work, layouts))],
            name=f"compose keyboard {mode}",
            timeout=10,
        )
        return result.returncode, result.stdout, result.stderr

    @pytest.mark.parametrize(
        ("layouts", "expected"),
        [
            (None, "pc+us+fplinux(function_keys)+group(alt_shift_toggle)"),
            ((), "pc+us+fplinux(function_keys)+group(alt_shift_toggle)"),
            ((".apk.8f2c",), "pc+us+fplinux(function_keys)+group(alt_shift_toggle)"),
            (("ru",), "pc+us+fplinux(function_keys)+ru:2+group(alt_shift_toggle)"),
            (
                ("ua", "ru", "by"),
                "pc+us+fplinux(function_keys)+by:2+ru:3+ua:4+group(alt_shift_toggle)",
            ),
            (("de_ch1",), "pc+us+fplinux(function_keys)+de_ch1:2+group(alt_shift_toggle)"),
        ],
        ids=["absent", "empty", ".apk.8f2c", "ru", "ua-ru-by", "de_ch1"],
    )
    def test_optional_layouts_follow_us_in_name_order(
        self, executable: Path, tmp_path: Path, layouts: tuple[str, ...] | None, expected: str
    ) -> None:
        """Each registered layout takes the next group after US, sorted by name."""
        assert (self.compose(executable, tmp_path, "symbols", layouts)) == ((0, expected, ""))

    @pytest.mark.parametrize(
        "name",
        ["Ru", "ru(phonetic)", "ru:2", "ru+us", 'ru"', "ru ua"],
        ids=["Ru", "ru-phonetic", "ru-2", "ru+us", "ru", "ru-ua"],
    )
    def test_layout_names_that_could_change_the_include_are_rejected(
        self, executable: Path, tmp_path: Path, name: str
    ) -> None:
        """Only lowercase letters, digits and '_' reach the XKB include string."""
        status, output, error = self.compose(executable, tmp_path, "symbols", ("ru", name))

        assert ((status, output)) == ((1, ""))
        assert ("invalid keyboard layout name") in (error)

    def test_a_fourth_optional_layout_is_refused(self, executable: Path, tmp_path: Path) -> None:
        """XKB has four groups, so US plus three optional layouts is the limit."""
        status, output, error = self.compose(
            executable, tmp_path, "symbols", ("by", "de", "ru", "ua")
        )

        assert ((status, output)) == ((1, ""))
        assert ("more than 3 optional keyboard layouts are installed") in (error)
