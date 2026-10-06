# SPDX-License-Identifier: GPL-2.0-only
"""Host behavior of the shared command-line library through a linked command."""

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

SHARED = ROOT / "include/fplinux"


class FPLinuxCliTests:
    """Use a command fixture to observe parsing, validation and borrowed argv."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the production library with a resource-free command fixture."""
        with ExitStack() as cleanup:
            cls.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(cls.temporary)
            cls.executable = Path(cls.temporary.name) / "cli-probe"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{SHARED}",
                    str(ROOT / "tests/host_tool/runtime/fplinux-cli.c"),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    "-o",
                    str(cls.executable),
                ],
                name="compile shared CLI fixture",
                timeout=30,
                check=True,
            )
            yield

    @pytest.mark.parametrize(
        "arguments",
        [
            ("-h",),
            ("-xh",),
            ("--he",),
            ("--unknown", "--help"),
            ("--input=", "--mode=invalid", "--help"),
            ("--input=one", "--mode=fast", "--mode=slow", "-h"),
        ],
        ids=[
            "h",
            "xh",
            "he",
            "unknown-help",
            "input-mode-invalid-help",
            "input-one-mode-fast-mode-slow-h",
        ],
    )
    def test_help_wins_without_invoking_command_validation(
        self, arguments: tuple[str, ...]
    ) -> None:
        """Recognized help is stable, stdout-only and precedes all validation."""
        expected = run_process(
            [str(self.executable), "values", "--help"], name="read CLI help", timeout=5
        ).stdout
        assert expected.startswith("Usage: cli-probe ")
        assert ("--input=PATH") in (expected)
        assert ("-h, --help") in (expected)
        result = run_process(
            [str(self.executable), "values", *arguments],
            name="check help precedence",
            timeout=5,
        )
        assert (result.returncode) == (0), result.stderr
        assert (result.stderr) == ("")
        assert (result.stdout) == (expected)

    @pytest.mark.parametrize(
        "arguments",
        [
            (),
            ("--input",),
            ("--input=",),
            ("--input=x", "--mode=fast", "--mode=slow"),
            ("--input=x", "--unknown"),
            ("--input=x", "--display=yes"),
            ("--input=x", "--help=yes"),
            ("--input=x", "--dis"),
            ("--input=x", "first", "second"),
        ],
        ids=[
            "empty",
            "input",
            "input-2",
            "input-x-mode-fast-mode-slow",
            "input-x-unknown",
            "input-x-display-yes",
            "input-x-help-yes",
            "input-x-dis",
            "input-x-first-second",
        ],
    )
    def test_syntax_and_cardinality_errors_precede_callbacks(
        self, arguments: tuple[str, ...]
    ) -> None:
        """Malformed input never reaches the command's option callback."""
        result = run_process(
            [str(self.executable), "values", *arguments],
            name="check syntax before callbacks",
            timeout=5,
        )
        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("Try 'cli-probe --help'") in (result.stderr)
        assert ("fixture callbacks=0") in (result.stderr)

    @pytest.mark.parametrize(
        ("options", "expected_hold"),
        [(("--display", "--display-m=0"), 0), (("--display-ms", "0", "--display"), 2000)],
        ids=["display-display-m-0-0", "display-ms-0-display-2000"],
    )
    def test_exact_names_prefixes_and_repeats_preserve_occurrence_order(
        self, options: tuple[str, ...], expected_hold: int
    ) -> None:
        """Exact display and unique display-ms prefixes select the right option."""
        result = run_process(
            [str(self.executable), "values", "--input=old", "--inp", "new", *options],
            name="check exact match and occurrence order",
            timeout=5,
        )
        assert (result.returncode) == (0), result.stderr
        assert ("input=new\ninput_count=2\n") in (result.stdout)
        assert (f"hold_ms={expected_hold}\n") in (result.stdout)

    def test_invalid_earlier_value_is_not_hidden_by_later_value(self) -> None:
        """Every supplied occurrence reaches command-owned value validation."""
        result = run_process(
            [str(self.executable), "values", "--input=x", "--display-ms=60001", "--display-ms=0"],
            name="check earlier invalid value",
            timeout=5,
        )
        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("display-ms must be in 0..60000") in (result.stderr)

    def test_interspersed_options_leave_the_original_argv_unchanged(self) -> None:
        """The caller retains the same ordering and strings after parsing."""
        result = run_process(
            [str(self.executable), "values", "operand", "--input", "file", "--display"],
            name="check borrowed argv ordering",
            timeout=5,
        )
        assert (result.returncode) == (0), result.stderr
        assert ("operand=operand\n") in (result.stdout)
        assert ("argv[1]=<operand>\nargv[2]=<--input>\nargv[3]=<file>\nargv[4]=<--display>\n") in (
            result.stdout
        )

    @pytest.mark.parametrize("value", ["--help", "--"], ids=["help", "separator"])
    def test_consumed_help_and_separator_remain_option_values(self, value: str) -> None:
        """Value-taking options consume option-shaped strings verbatim."""
        result = run_process(
            [str(self.executable), "values", "--input", value, "--display"],
            name="check option-shaped value",
            timeout=5,
        )
        assert (result.returncode) == (0), result.stderr
        assert (f"input={value}\n") in (result.stdout)
        assert ("hold_ms=2000\n") in (result.stdout)
        assert ("Usage:") not in (result.stdout)

    def test_separator_stops_options_but_keeps_positionals(self) -> None:
        """A help-shaped operand after -- is an operand, not a help request."""
        result = run_process(
            [str(self.executable), "values", "--input=x", "--", "--help"],
            name="check positional after separator",
            timeout=5,
        )
        assert (result.returncode) == (0), result.stderr
        assert ("operand=--help\n") in (result.stdout)
        assert ("Usage:") not in (result.stdout)

    def test_opaque_tail_preserves_empty_and_option_shaped_arguments(self) -> None:
        """Tail consumers receive the original untouched suffix after --."""
        result = run_process(
            [
                str(self.executable),
                "tail",
                "--input=x",
                "--",
                "tool",
                "--help",
                "",
                "--mode=bad",
                "--",
            ],
            name="check opaque command tail",
            timeout=5,
        )
        assert (result.returncode) == (0), result.stderr
        assert ("tail_index=3\n") in (result.stdout)
        assert (
            "tail[0]=<tool>\ntail[1]=<--help>\ntail[2]=<>\ntail[3]=<--mode=bad>\ntail[4]=<-->\n"
        ) in (result.stdout)
