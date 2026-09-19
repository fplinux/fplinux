# SPDX-License-Identifier: GPL-2.0-only
"""Host behavior of the shared command-line library through a linked command."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
SHARED = ROOT / "alpine/shared"


class FPLinuxCliTests(unittest.TestCase):
    """Use a command fixture to observe parsing, validation and borrowed argv."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Link the production library with a resource-free command fixture."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
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
                str(ROOT / "tests/host_tool/fplinux-cli.c"),
                str(SHARED / "fplinux-cli.c"),
                "-o",
                str(cls.executable),
            ],
            name="compile shared CLI fixture",
            timeout=30,
            check=True,
        )

    def test_help_wins_without_invoking_command_validation(self) -> None:
        """Recognized help is stable, stdout-only and precedes all validation."""
        expected = run_process(
            [str(self.executable), "values", "--help"], name="read CLI help", timeout=5
        ).stdout
        self.assertTrue(expected.startswith("Usage: cli-probe "))
        self.assertIn("--input=PATH", expected)
        self.assertIn("-h, --help", expected)
        for arguments in (
            ("-h",),
            ("-xh",),
            ("--he",),
            ("--unknown", "--help"),
            ("--input=", "--mode=invalid", "--help"),
            ("--input=one", "--mode=fast", "--mode=slow", "-h"),
        ):
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), "values", *arguments],
                    name="check help precedence",
                    timeout=5,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertEqual(result.stdout, expected)

    def test_syntax_and_cardinality_errors_precede_callbacks(self) -> None:
        """Malformed input never reaches the command's option callback."""
        for arguments in (
            (),
            ("--input",),
            ("--input=",),
            ("--input=x", "--mode=fast", "--mode=slow"),
            ("--input=x", "--unknown"),
            ("--input=x", "--display=yes"),
            ("--input=x", "--help=yes"),
            ("--input=x", "--dis"),
            ("--input=x", "first", "second"),
        ):
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), "values", *arguments],
                    name="check syntax before callbacks",
                    timeout=5,
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("Try 'cli-probe --help'", result.stderr)
                self.assertIn("fixture callbacks=0", result.stderr)

    def test_exact_names_prefixes_and_repeats_preserve_occurrence_order(self) -> None:
        """Exact display and unique display-ms prefixes select the right option."""
        for options, expected_hold in (
            (("--display", "--display-m=0"), 0),
            (("--display-ms", "0", "--display"), 2000),
        ):
            with self.subTest(options=options):
                result = run_process(
                    [str(self.executable), "values", "--input=old", "--inp", "new", *options],
                    name="check exact match and occurrence order",
                    timeout=5,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("input=new\ninput_count=2\n", result.stdout)
                self.assertIn(f"hold_ms={expected_hold}\n", result.stdout)

    def test_invalid_earlier_value_is_not_hidden_by_later_value(self) -> None:
        """Every supplied occurrence reaches command-owned value validation."""
        result = run_process(
            [str(self.executable), "values", "--input=x", "--display-ms=60001", "--display-ms=0"],
            name="check earlier invalid value",
            timeout=5,
        )
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("display-ms must be in 0..60000", result.stderr)

    def test_interspersed_options_leave_the_original_argv_unchanged(self) -> None:
        """The caller retains the same ordering and strings after parsing."""
        result = run_process(
            [str(self.executable), "values", "operand", "--input", "file", "--display"],
            name="check borrowed argv ordering",
            timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("operand=operand\n", result.stdout)
        self.assertIn(
            "argv[1]=<operand>\nargv[2]=<--input>\nargv[3]=<file>\nargv[4]=<--display>\n",
            result.stdout,
        )

    def test_consumed_help_and_separator_remain_option_values(self) -> None:
        """Value-taking options consume option-shaped strings verbatim."""
        for value in ("--help", "--"):
            with self.subTest(value=value):
                result = run_process(
                    [str(self.executable), "values", "--input", value, "--display"],
                    name="check option-shaped value",
                    timeout=5,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"input={value}\n", result.stdout)
                self.assertIn("hold_ms=2000\n", result.stdout)
                self.assertNotIn("Usage:", result.stdout)

    def test_separator_stops_options_but_keeps_positionals(self) -> None:
        """A help-shaped operand after -- is an operand, not a help request."""
        result = run_process(
            [str(self.executable), "values", "--input=x", "--", "--help"],
            name="check positional after separator",
            timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("operand=--help\n", result.stdout)
        self.assertNotIn("Usage:", result.stdout)

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
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("tail_index=3\n", result.stdout)
        self.assertIn(
            "tail[0]=<tool>\ntail[1]=<--help>\ntail[2]=<>\ntail[3]=<--mode=bad>\ntail[4]=<-->\n",
            result.stdout,
        )


if __name__ == "__main__":
    unittest.main()
