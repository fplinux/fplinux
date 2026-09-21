# SPDX-License-Identifier: GPL-2.0-only
"""Characterize image-tool arguments with host executables and local files."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]
APORTS = ROOT / "alpine/aports"
SHARED = ROOT / "include/fplinux"
TOOLS = ("fplinux-rotate", "fplinux-jpeg", "fplinux-jpeg-cpu", "fplinux-present")


class ImageToolCliTests(unittest.TestCase):
    """Exercise argument handling without opening a phone or host device."""

    directory: Path

    @classmethod
    def setUpClass(cls) -> None:
        """Compile complete programs using the check image's libjpeg8 dependency."""
        temporary = tempfile.TemporaryDirectory(prefix="fplinux-image-cli-", dir="/tmp")
        cls.addClassCleanup(temporary.cleanup)
        cls.directory = Path(temporary.name)
        sources = {
            "fplinux-rotate": [
                APORTS / "fplinux-rotate/fplinux-rotate.c",
                APORTS / "fplinux-rotate/fplinux-rotate-core.c",
                ROOT / "lib/fplinux/fplinux-fb-session.c",
            ],
            "fplinux-jpeg": [APORTS / "fplinux-jpeg/fplinux-jpeg.c"],
            "fplinux-jpeg-cpu": [APORTS / "fplinux-jpeg/fplinux-jpeg-cpu.c"],
            "fplinux-present": [
                APORTS / "fplinux-present/fplinux-present.c",
                ROOT / "lib/fplinux/fplinux-fb-session.c",
            ],
            "fplinux-rotate-display": [
                APORTS / "fplinux-rotate/fplinux-rotate.c",
                APORTS / "fplinux-rotate/fplinux-rotate-core.c",
                ROOT / "tests/host_tool/fixtures/rotation-display.c",
            ],
        }
        for tool, tool_sources in sources.items():
            libraries = ["-ljpeg"] if tool == "fplinux-jpeg-cpu" else []
            run_process(
                [
                    "cc",
                    "-O2",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{SHARED}",
                    f"-I{ROOT / 'platforms/ums9117/linux/include/uapi/fplinux'}",
                    *(str(source) for source in tool_sources),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    *libraries,
                    "-o",
                    str(cls.directory / tool),
                ],
                name=f"compile {tool} for host argument checks",
                timeout=30,
                check=True,
            )

    def run_tool(self, tool: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run a bounded command in a private directory with a fixed locale."""
        device_paths = (
            ["--tty", "missing-tty", "--framebuffer", "missing-framebuffer"]
            if tool == "fplinux-present"
            else []
        )
        return run_process(
            [str(self.directory / tool), *device_paths, *arguments],
            name=f"check {tool} arguments",
            timeout=10,
            cwd=self.directory,
            env={"LC_ALL": "C", "TZ": "UTC"},
        )

    def assert_usage(
        self, tool: str, *arguments: str, success: bool
    ) -> subprocess.CompletedProcess[str]:
        """Help and syntax errors have distinct statuses and output streams."""
        result = self.run_tool(tool, *arguments)
        self.assertEqual(result.returncode, 0 if success else 2, result.stderr)
        if success:
            self.assertTrue(result.stdout.startswith("Usage:"), result.stdout)
            self.assertIn("--help", result.stdout)
            self.assertIn("--input", result.stdout)
            self.assertEqual(result.stderr, "")
        else:
            self.assertEqual(result.stdout, "")
            self.assertIn("--help", result.stderr)
        return result

    def test_help_is_stable_and_precedes_validation(self) -> None:
        """Recognized help succeeds independently of other options and their values."""
        for tool in TOOLS:
            baseline = self.assert_usage(tool, "--help", success=True)
            for arguments in (("-h",), ("--unknown", "garbage", "-h")):
                with self.subTest(tool=tool, arguments=arguments):
                    result = self.assert_usage(tool, *arguments, success=True)
                    self.assertEqual(result.stdout, baseline.stdout)

    def test_malformed_options_and_positionals_are_rejected(self) -> None:
        """Unknown options, missing values and unsupported positional input are errors."""
        for tool in TOOLS:
            for arguments in (
                ("--help=yes",),
                ("--unknown",),
                ("file",),
                ("",),
                ("-",),
                ("--",),
                ("--", "--help"),
                ("--input", "file", "--", "extra"),
                ("--input",),
                ("--input", "file", "--output"),
            ):
                with self.subTest(tool=tool, arguments=arguments):
                    self.assert_usage(tool, *arguments, success=False)

    def test_option_shaped_path_values_are_consumed_before_help(self) -> None:
        """A required path consumes its next token, even when it looks like an option."""
        for tool in TOOLS:
            for value in ("--help", "--", "-file", ""):
                with self.subTest(tool=tool, value=value):
                    self.assert_usage(tool, "--input", value, "--help", success=True)
            with self.subTest(tool=tool, scenario="consumed help is not executed"):
                self.assert_usage(tool, "--input", "--help", "--output", success=False)

    def test_attached_values_and_prefixes_reach_local_file_errors(self) -> None:
        """Native option spellings pass real validation before missing local resources."""
        for tool, arguments in (
            (
                "fplinux-rotate",
                ("--eng=cpu", "--format=grey", "--width=3", "--height=2", "--in=missing"),
            ),
            ("fplinux-jpeg", ("--ope=decode", "--in=missing", "--out=unused")),
            ("fplinux-jpeg-cpu", ("--ope=decode", "--in=missing", "--out=unused")),
            ("fplinux-present", ("--mo=cpu-rgb565", "--in=missing")),
        ):
            with self.subTest(tool=tool):
                result = self.run_tool(tool, *arguments)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertTrue(result.stderr.startswith(f"{tool}: "), result.stderr)
                self.assertNotIn("--help' for more information", result.stderr)

    def test_complete_arguments_reject_positionals_after_a_separator(self) -> None:
        """A separator ends options but does not introduce an input-file positional."""
        for tool, arguments in (
            (
                "fplinux-rotate",
                ("--engine", "cpu", "--format", "grey", "--width", "3", "--height", "2"),
            ),
            ("fplinux-jpeg", ("--output", "unused-output")),
            ("fplinux-jpeg-cpu", ("--operation", "decode", "--output", "unused-output")),
            ("fplinux-present", ()),
        ):
            plain = self.run_tool(tool, *arguments, "--input", "missing-input")
            delimited = self.run_tool(tool, *arguments, "--input", "missing-input", "--")
            self.assertEqual(plain.returncode, 1, plain.stderr)
            self.assertEqual(delimited.returncode, plain.returncode, delimited.stderr)
            self.assertEqual(delimited.stderr, plain.stderr)
            for trailing in (("extra",), ("--", "extra")):
                with self.subTest(tool=tool, trailing=trailing):
                    self.assert_usage(
                        tool, *arguments, "--input", "missing-input", *trailing, success=False
                    )

    def test_help_precedes_numeric_and_final_checks(self) -> None:
        """Domain validation cannot start work when an actual help option is present."""
        for tool, option, invalid, deferred in (
            ("fplinux-rotate", "--width", "bad", "0"),
            ("fplinux-jpeg", "--width", "0", "2"),
            ("fplinux-jpeg-cpu", "--width", "0", "3"),
            ("fplinux-present", "--fps", "0", "1"),
        ):
            for value in (invalid, "--help", "--", "-1"):
                with self.subTest(tool=tool, value=value):
                    self.assert_usage(tool, option, value, "--help", success=True)
            with self.subTest(tool=tool, scenario="deferred validation"):
                self.assert_usage(tool, option, deferred, "--help", success=True)
        self.assert_usage("fplinux-present", "--hold-ms", "-0", "--help", success=True)

    def test_negative_unsigned_values_fail_before_resource_access(self) -> None:
        """A minus sign cannot wrap into an accepted scalar or crop coordinate."""
        cases = (
            (
                "fplinux-present",
                ("--input", "missing-input", "--hold-ms", " -0"),
                "--hold-ms must be an integer from 0 to 60000",
            ),
            (
                "fplinux-rotate",
                (
                    "--engine",
                    "cpu",
                    "--format",
                    "grey",
                    "--width",
                    "3",
                    "--height",
                    "2",
                    "--crop",
                    "-0,0,1,1",
                    "--input",
                    "missing-input",
                ),
                "invalid option value or combination",
            ),
        )
        for tool, arguments, error in cases:
            with self.subTest(tool=tool):
                result = self.assert_usage(tool, *arguments, success=False)
                self.assertIn(error, result.stderr)

    def test_repeated_flags_keep_help_available(self) -> None:
        """Repeated flags and conflicting values still allow explicit help."""
        self.assert_usage("fplinux-jpeg", "--timing", "--timing", "--help", success=True)
        for arguments in (
            ("--hflip", "--hflip", "--vflip", "--verify", "--verify"),
            ("--display", "--display-ms", "0"),
            ("--display-ms", "0", "--display"),
        ):
            with self.subTest(arguments=arguments):
                self.assert_usage("fplinux-rotate", *arguments, "--help", success=True)
        self.assert_usage(
            "fplinux-rotate", "--display-ms", "60001", "--display", "--help", success=True
        )

    def test_invalid_earlier_values_are_not_hidden_by_later_valid_values(self) -> None:
        """Last-wins assignment still validates each supplied numeric or enum value."""
        for tool, base, option, invalid, valid in (
            (
                "fplinux-rotate",
                ("--engine", "cpu", "--format", "grey", "--height", "2"),
                "--width",
                "bad",
                "3",
            ),
            ("fplinux-jpeg", ("--output", "unused"), "--scale", "3", "1"),
            (
                "fplinux-jpeg-cpu",
                ("--operation", "decode", "--output", "unused"),
                "--scale",
                "3",
                "2",
            ),
            ("fplinux-present", (), "--mode", "invalid", "nv16"),
        ):
            with self.subTest(tool=tool, option=option):
                prefix = (*base, "--input", "missing-input")
                accepted = self.run_tool(tool, *prefix, option, valid)
                self.assertEqual(accepted.returncode, 1, accepted.stderr)
                self.assert_usage(tool, *prefix, option, invalid, option, valid, success=False)

    def test_rotate_last_values_control_cpu_result_and_iterations(self) -> None:
        """Duplicate engine, format, dimensions, transform and output use their last value."""
        source = self.directory / "source.grey"
        output = self.directory / "rotated.grey"
        unused_output = self.directory / "unused.grey"
        source.write_bytes(bytes([1, 2, 3, 4, 5, 6]))
        result = self.run_tool(
            "fplinux-rotate",
            "--engine",
            "rota",
            "--engine",
            "cpu",
            "--device",
            str(self.directory / "missing-video"),
            "--format",
            "rgb565",
            "--format",
            "grey",
            "--width",
            "2",
            "--width",
            "3",
            "--height",
            "2",
            "--rotate",
            "180",
            "--rotate",
            "90",
            "--iterations",
            "1",
            "--iterations",
            "2",
            "--input",
            str(self.directory / "missing-input"),
            "--input",
            str(source),
            "--output",
            str(unused_output),
            "--output",
            str(output),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertIn("benchmark stage=selected engine=cpu iterations=2", result.stdout)
        self.assertEqual(output.read_bytes(), bytes([4, 1, 5, 2, 6, 3]))
        self.assertFalse(unused_output.exists())

    def test_rotate_display_order_reaches_the_stubbed_delay_boundary(self) -> None:
        """The last display option selects zero, explicit, or default preview duration."""
        source = self.directory / "display.grey"
        output = self.directory / "display-output.grey"
        source.write_bytes(bytes([1, 2, 3, 4, 5, 6]))
        base = (
            "--engine",
            "cpu",
            "--format",
            "grey",
            "--width",
            "3",
            "--height",
            "2",
            "--input",
            str(source),
            "--output",
            str(output),
        )
        for arguments, expected_holds in (
            (("--display", "--display-ms=0"), []),
            (("--display-ms=0", "--display"), ["stub hold_us=2000000"]),
            (("--display", "--display-m=17"), ["stub hold_us=17000"]),
            (("--display-ms=17", "--display", "--display-ms=0"), []),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_tool("fplinux-rotate-display", *base, *arguments)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("stub preview presented", result.stdout)
                holds = [line for line in result.stdout.splitlines() if "hold_us=" in line]
                self.assertEqual(holds, expected_holds)

    def test_jpeg_last_operation_and_input_reach_the_local_file_error(self) -> None:
        """The final decode operation accepts missing geometry and uses the last input."""
        for tool, error in (
            ("fplinux-jpeg", "fplinux-jpeg: input: No such file or directory\n"),
            (
                "fplinux-jpeg-cpu",
                "fplinux-jpeg-cpu: cannot read bounded regular input: missing-jpeg\n",
            ),
        ):
            with self.subTest(tool=tool):
                result = self.run_tool(
                    tool,
                    "--operation",
                    "encode",
                    "--operation",
                    "decode",
                    "--input",
                    str(self.directory),
                    "--input",
                    "missing-jpeg",
                    "--output",
                    str(self.directory / "unused.nv16"),
                )
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertEqual(result.stderr, error)

    def test_present_last_mode_and_missing_tty_report_local_error(self) -> None:
        """The last mode controls output eligibility; a missing local tty stops execution."""
        base = (
            "--input",
            "missing-input",
            "--output",
            "unused.rgb565",
            "--tty",
            str(self.directory),
            "--tty",
            "missing-tty",
            "--framebuffer",
            "missing-framebuffer",
        )
        result = self.run_tool("fplinux-present", *base, "--mode", "nv16", "--mode", "cpu-rgb565")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertEqual(
            result.stderr, "fplinux-present: cannot open console: No such file or directory\n"
        )
        self.assert_usage(
            "fplinux-present", *base, "--mode", "cpu-rgb565", "--mode", "nv16", success=False
        )


if __name__ == "__main__":
    unittest.main()
