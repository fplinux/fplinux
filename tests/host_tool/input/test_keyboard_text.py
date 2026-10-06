# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for keyboard layout selection.

The C driver links the shared layout and text-filter source. It does not
compile a keymap with xkbcommon or open an input device.
"""

from __future__ import annotations

import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SHARED_INCLUDE = ROOT / "include/fplinux"
SOURCE = ROOT / "lib/fplinux/fplinux-keyboard-text.c"
HARNESS = ROOT / "tests/host_tool/input/fplinux-keyboard-text.c"


class KeyboardTextTests:
    """Compose layouts from temporary data roots with literal layout files."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Compile the host driver once for this test class."""
        with ExitStack() as cleanup:
            cls.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(cls.temporary)
            cls.executable = Path(cls.temporary.name) / "fplinux-keyboard-text"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(SHARED_INCLUDE),
                    str(HARNESS),
                    str(SOURCE),
                    "-o",
                    str(cls.executable),
                ],
                name="compile shared keyboard text driver",
                timeout=30,
                check=True,
            )
            yield

    @pytest.fixture(autouse=True)
    def _case_directory(self, tmp_path: Path) -> None:
        """Keep each layout directory separate from the compiled driver."""
        self.work = tmp_path

    def data_root(self, layouts: tuple[str, ...] | None) -> Path:
        """Create a data root whose layouts directory holds empty named files."""
        root = Path(tempfile.mkdtemp(dir=self.work))
        if layouts is not None:
            (root / "layouts").mkdir()
            for layout in layouts:
                (root / "layouts" / layout).write_bytes(b"")
        return root

    def compose(self, mode: str, layouts: tuple[str, ...] | None) -> tuple[int, str, str]:
        """Run the driver for one data root and return status, stdout and stderr."""
        result = run_process(
            [str(self.executable), mode, str(self.data_root(layouts))],
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
        self, layouts: tuple[str, ...] | None, expected: str
    ) -> None:
        """Each registered layout takes the next group after US, sorted by name."""
        assert (self.compose("symbols", layouts)) == ((0, expected, ""))

    @pytest.mark.parametrize(
        "name",
        ["Ru", "ru(phonetic)", "ru:2", "ru+us", 'ru"', "ru ua"],
        ids=["Ru", "ru-phonetic", "ru-2", "ru+us", "ru", "ru-ua"],
    )
    def test_layout_names_that_could_change_the_include_are_rejected(self, name: str) -> None:
        """Only lowercase letters, digits and '_' reach the XKB include string."""
        status, output, error = self.compose("symbols", ("ru", name))

        assert ((status, output)) == ((1, ""))
        assert ("invalid keyboard layout name") in (error)

    def test_a_fourth_optional_layout_is_refused(self) -> None:
        """XKB has four groups, so US plus three optional layouts is the limit."""
        status, output, error = self.compose("symbols", ("by", "de", "ru", "ua"))

        assert ((status, output)) == ((1, ""))
        assert ("more than 3 optional keyboard layouts are installed") in (error)

    @pytest.mark.parametrize(
        ("text", "printable"),
        [
            ("a", True),
            ("Z", True),
            (" ", True),
            ("ж", True),
            ("€", True),
            ("\xa0", True),
            ("", False),
            ("\r", False),
            ("\x08", False),
            ("\t", False),
            ("\x1b", False),
            ("\x7f", False),
            ("\x01", False),
            ("\x85", False),
            ("a\x1b", False),
        ],
        ids=[
            "ascii-lower",
            "ascii-upper",
            "space",
            "cyrillic",
            "euro",
            "no-break-space",
            "empty",
            "carriage-return",
            "backspace",
            "tab",
            "escape",
            "delete",
            "ctrl-a",
            "c1-control",
            "embedded-escape",
        ],
    )
    def test_control_characters_are_not_text(self, text: str, *, printable: bool) -> None:
        """Enter, Backspace, Tab, Escape, DEL and Ctrl combinations insert nothing."""
        result = run_process(
            [str(self.executable), "printable", text],
            name="classify keyboard text",
            timeout=10,
        )

        assert (result.returncode) == (0 if printable else 1)
