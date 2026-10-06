# SPDX-License-Identifier: GPL-2.0-only
"""Characterize image-tool arguments with host executables and local files."""

from __future__ import annotations

import shlex
import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator

APORTS = ROOT / "alpine/aports"
SHARED = ROOT / "include/fplinux"
TOOLS = ("fplinux-rotate", "fplinux-jpeg", "fplinux-jpeg-cpu", "fplinux-present")


class ImageToolCliTests:
    """Exercise argument handling without opening a phone or host device."""

    directory: Path
    executables_directory: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Compile complete programs using the check image's libjpeg8 dependency."""
        with ExitStack() as cleanup:
            temporary = tempfile.TemporaryDirectory(prefix="fplinux-image-cli-", dir="/tmp")
            cleanup.enter_context(temporary)
            cls.executables_directory = Path(temporary.name)
            sources = {
                "fplinux-rotate": [
                    APORTS / "fplinux-rotate/fplinux-rotate.c",
                    APORTS / "fplinux-rotate/fplinux-rotate-core.c",
                    APORTS / "fplinux-rotate/rotate-rota.c",
                    ROOT / "lib/fplinux/fplinux-drm-session.c",
                ],
                "fplinux-jpeg": [
                    APORTS / "fplinux-jpeg/fplinux-jpeg.c",
                    APORTS / "fplinux-jpeg/jpeg-v4l2.c",
                ],
                "fplinux-jpeg-cpu": [
                    APORTS / "fplinux-jpeg/fplinux-jpeg-cpu.c",
                    APORTS / "fplinux-jpeg/jpeg-cpu-codec.c",
                ],
                "fplinux-present": [
                    APORTS / "fplinux-present/fplinux-present.c",
                    ROOT / "lib/fplinux/fplinux-drm-session.c",
                ],
                "fplinux-rotate-display": [
                    APORTS / "fplinux-rotate/fplinux-rotate.c",
                    APORTS / "fplinux-rotate/fplinux-rotate-core.c",
                    APORTS / "fplinux-rotate/rotate-rota.c",
                    ROOT / "tests/host_tool/build/fixtures/rotation-display.c",
                ],
            }
            drm_flags = shlex.split(
                run_process(
                    ["pkg-config", "--cflags", "--libs", "libdrm"],
                    name="read DRM compiler and linker flags",
                    timeout=10,
                    check=True,
                ).stdout
            )
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
                        *drm_flags,
                        "-o",
                        str(cls.executables_directory / tool),
                    ],
                    name=f"compile {tool} for host argument checks",
                    timeout=30,
                    check=True,
                )
            yield

    @pytest.fixture(autouse=True)
    def _case_directory(self, tmp_path: Path) -> None:
        """Give each invocation its own input and output files."""
        self.directory = tmp_path

    def run_tool(self, tool: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run a bounded command in a private directory with a fixed locale."""
        device_paths = (
            ["--tty", "missing-tty", "--drm", "missing-drm"] if tool == "fplinux-present" else []
        )
        return run_process(
            [str(self.executables_directory / tool), *device_paths, *arguments],
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
        assert (result.returncode) == (0 if success else 2), result.stderr
        if success:
            assert result.stdout.startswith("Usage:"), result.stdout
            assert ("--help") in (result.stdout)
            assert ("--input") in (result.stdout)
            assert (result.stderr) == ("")
        else:
            assert (result.stdout) == ("")
            assert ("--help") in (result.stderr)
        return result

    @pytest.mark.parametrize("tool", TOOLS, ids=str)
    @pytest.mark.parametrize(
        "arguments", [("-h",), ("--unknown", "garbage", "-h")], ids=["h", "unknown-garbage-h"]
    )
    def test_help_is_stable_and_precedes_validation(
        self, arguments: tuple[str, ...], tool: str
    ) -> None:
        """Recognized help succeeds independently of other options and their values."""
        baseline = self.assert_usage(tool, "--help", success=True)
        result = self.assert_usage(tool, *arguments, success=True)
        assert (result.stdout) == (baseline.stdout)

    @pytest.mark.parametrize("tool", TOOLS, ids=str)
    @pytest.mark.parametrize(
        "arguments",
        [
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
        ],
        ids=[
            "help-yes",
            "unknown",
            "file",
            "empty",
            "empty-2",
            "separator",
            "separator-help",
            "input-file-separator-extra",
            "input",
            "input-file-output",
        ],
    )
    def test_malformed_options_and_positionals_are_rejected(
        self, arguments: tuple[str, ...], tool: str
    ) -> None:
        """Unknown options, missing values and unsupported positional input are errors."""
        self.assert_usage(tool, *arguments, success=False)

    @pytest.mark.parametrize("tool", TOOLS, ids=str)
    @pytest.mark.parametrize(
        "value", ["--help", "--", "-file", ""], ids=["help", "separator", "file", "empty"]
    )
    def test_option_shaped_path_values_are_consumed_before_help(
        self, value: str, tool: str
    ) -> None:
        """A required path consumes its next token, even when it looks like an option."""
        self.assert_usage(tool, "--input", value, "--help", success=True)
        self.assert_usage(tool, "--input", "--help", "--output", success=False)

    @pytest.mark.parametrize(
        ("tool", "arguments"),
        [
            (
                "fplinux-rotate",
                ("--eng=cpu", "--format=grey", "--width=3", "--height=2", "--in=missing"),
            ),
            ("fplinux-jpeg", ("--ope=decode", "--in=missing", "--out=unused")),
            ("fplinux-jpeg-cpu", ("--ope=decode", "--in=missing", "--out=unused")),
            ("fplinux-present", ("--mo=cpu-rgb565", "--in=missing")),
        ],
        ids=[
            "fplinux-rotate-eng-cpu-format-grey-width-3-height-2-in-missing",
            "fplinux-jpeg-ope-decode-in-missing-out-unused",
            "fplinux-jpeg-cpu-ope-decode-in-missing-out-unused",
            "fplinux-present-mo-cpu-rgb565-in-missing",
        ],
    )
    def test_attached_values_and_prefixes_reach_local_file_errors(
        self, tool: str, arguments: tuple[str, ...]
    ) -> None:
        """Native option spellings pass real validation before missing local resources."""
        result = self.run_tool(tool, *arguments)
        assert (result.returncode) == (1), result.stderr
        assert result.stderr.startswith(f"{tool}: "), result.stderr
        assert ("--help' for more information") not in (result.stderr)

    @pytest.mark.parametrize(
        ("tool", "arguments"),
        [
            (
                "fplinux-rotate",
                ("--engine", "cpu", "--format", "grey", "--width", "3", "--height", "2"),
            ),
            ("fplinux-jpeg", ("--output", "unused-output")),
            ("fplinux-jpeg-cpu", ("--operation", "decode", "--output", "unused-output")),
            ("fplinux-present", ()),
        ],
        ids=[
            "fplinux-rotate-engine-cpu-format-grey-width-3-height-2",
            "fplinux-jpeg-output-unused-output",
            "fplinux-jpeg-cpu-operation-decode-output-unused-output",
            "fplinux-present-empty",
        ],
    )
    @pytest.mark.parametrize(
        "trailing", [("extra",), ("--", "extra")], ids=["extra", "separator-extra"]
    )
    def test_complete_arguments_reject_positionals_after_a_separator(
        self, trailing: tuple[str, ...], tool: str, arguments: tuple[str, ...]
    ) -> None:
        """A separator ends options but does not introduce an input-file positional."""
        plain = self.run_tool(tool, *arguments, "--input", "missing-input")
        delimited = self.run_tool(tool, *arguments, "--input", "missing-input", "--")
        assert (plain.returncode) == (1), plain.stderr
        assert (delimited.returncode) == (plain.returncode), delimited.stderr
        assert (delimited.stderr) == (plain.stderr)
        self.assert_usage(tool, *arguments, "--input", "missing-input", *trailing, success=False)

    @pytest.mark.parametrize(
        ("tool", "option", "value"),
        [
            pytest.param("fplinux-rotate", "--width", "bad", id="fplinux-rotate-width-0"),
            pytest.param("fplinux-rotate", "--width", "--help", id="fplinux-rotate-width-1"),
            pytest.param("fplinux-rotate", "--width", "--", id="fplinux-rotate-width-2"),
            pytest.param("fplinux-rotate", "--width", "-1", id="fplinux-rotate-width-3"),
            pytest.param("fplinux-rotate", "--width", "0", id="fplinux-rotate-width-4"),
            pytest.param("fplinux-jpeg", "--width", "0", id="fplinux-jpeg-width-0"),
            pytest.param("fplinux-jpeg", "--width", "--help", id="fplinux-jpeg-width-1"),
            pytest.param("fplinux-jpeg", "--width", "--", id="fplinux-jpeg-width-2"),
            pytest.param("fplinux-jpeg", "--width", "-1", id="fplinux-jpeg-width-3"),
            pytest.param("fplinux-jpeg", "--width", "2", id="fplinux-jpeg-width-4"),
            pytest.param("fplinux-jpeg-cpu", "--width", "0", id="fplinux-jpeg-cpu-width-0"),
            pytest.param("fplinux-jpeg-cpu", "--width", "--help", id="fplinux-jpeg-cpu-width-1"),
            pytest.param("fplinux-jpeg-cpu", "--width", "--", id="fplinux-jpeg-cpu-width-2"),
            pytest.param("fplinux-jpeg-cpu", "--width", "-1", id="fplinux-jpeg-cpu-width-3"),
            pytest.param("fplinux-jpeg-cpu", "--width", "3", id="fplinux-jpeg-cpu-width-4"),
            pytest.param("fplinux-present", "--fps", "0", id="fplinux-present-fps-0"),
            pytest.param("fplinux-present", "--fps", "--help", id="fplinux-present-fps-1"),
            pytest.param("fplinux-present", "--fps", "--", id="fplinux-present-fps-2"),
            pytest.param("fplinux-present", "--fps", "-1", id="fplinux-present-fps-3"),
            pytest.param("fplinux-present", "--fps", "1", id="fplinux-present-fps-4"),
            pytest.param(
                "fplinux-present", "--hold-ms", "-0", id="fplinux-present-hold-ms-negative-zero"
            ),
        ],
    )
    def test_help_precedes_numeric_and_final_checks(
        self, tool: str, option: str, value: str
    ) -> None:
        """Domain validation cannot start work when an actual help option is present."""
        self.assert_usage(tool, option, value, "--help", success=True)

    @pytest.mark.parametrize(
        ("tool", "arguments", "error"),
        [
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
        ],
        ids=[
            "fplinux-present-input-missing-input-hold-ms-negative-zero",
            "fplinux-rotate-engine-cpu-format-grey-width-3-height-2-crop-negative-zero-0-1-1-input-missing-input",
        ],
    )
    def test_negative_unsigned_values_fail_before_resource_access(
        self, tool: str, arguments: tuple[str, ...], error: str
    ) -> None:
        """A minus sign cannot wrap into an accepted scalar or crop coordinate."""
        result = self.assert_usage(tool, *arguments, success=False)
        assert (error) in (result.stderr)

    @pytest.mark.parametrize(
        "arguments",
        [
            ("--hflip", "--hflip", "--vflip", "--verify", "--verify"),
            ("--display", "--display-ms", "0"),
            ("--display-ms", "0", "--display"),
        ],
        ids=["hflip-hflip-vflip-verify-verify", "display-display-ms-0", "display-ms-0-display"],
    )
    def test_repeated_flags_keep_help_available(self, arguments: tuple[str, ...]) -> None:
        """Repeated flags and conflicting values still allow explicit help."""
        self.assert_usage("fplinux-jpeg", "--timing", "--timing", "--help", success=True)
        self.assert_usage("fplinux-rotate", *arguments, "--help", success=True)
        self.assert_usage(
            "fplinux-rotate", "--display-ms", "60001", "--display", "--help", success=True
        )

    @pytest.mark.parametrize(
        ("tool", "base", "option", "invalid", "valid"),
        [
            (
                "fplinux-rotate",
                ("--engine", "cpu", "--format", "grey", "--height", "2"),
                "--width",
                "bad",
                "3",
            ),
            ("fplinux-jpeg", ("--output", "unused"), "--scale", "3", "1"),
            (
                "fplinux-jpeg",
                ("--operation", "encode", "--width", "4", "--height", "2", "--output", "unused"),
                "--quality",
                "0",
                "85",
            ),
            (
                "fplinux-jpeg-cpu",
                ("--operation", "decode", "--output", "unused"),
                "--scale",
                "3",
                "2",
            ),
            ("fplinux-present", (), "--mode", "invalid", "nv16"),
        ],
        ids=[
            "fplinux-rotate-engine-cpu-format-grey-height-2-width-bad-3",
            "fplinux-jpeg-output-unused-scale-3-1",
            "fplinux-jpeg-operation-encode-width-4-height-2-output-unused-quality-0-85",
            "fplinux-jpeg-cpu-operation-decode-output-unused-scale-3-2",
            "fplinux-present-empty-mode-invalid-nv16",
        ],
    )
    def test_invalid_earlier_values_are_not_hidden_by_later_valid_values(
        self, tool: str, base: tuple[str, ...], option: str, invalid: str, valid: str
    ) -> None:
        """Last-wins assignment still validates each supplied numeric or enum value."""
        prefix = (*base, "--input", "missing-input")
        accepted = self.run_tool(tool, *prefix, option, valid)
        assert (accepted.returncode) == (1), accepted.stderr
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
        assert (result.returncode) == (0), result.stderr
        assert (result.stderr) == ("")
        assert ("benchmark stage=selected engine=cpu iterations=2") in (result.stdout)
        assert (output.read_bytes()) == (bytes([4, 1, 5, 2, 6, 3]))
        assert not (unused_output.exists())

    @pytest.mark.parametrize(
        ("arguments", "expected_holds"),
        [
            (("--display", "--display-ms=0"), []),
            (("--display-ms=0", "--display"), ["stub hold_us=2000000"]),
            (("--display", "--display-m=17"), ["stub hold_us=17000"]),
            (("--display-ms=17", "--display", "--display-ms=0"), []),
        ],
        ids=[
            "display-display-ms-0",
            "display-ms-0-display",
            "display-display-m-17",
            "display-ms-17-display-display-ms-0",
        ],
    )
    def test_rotate_display_order_reaches_the_stubbed_delay_boundary(
        self, arguments: tuple[str, ...], expected_holds: list[str]
    ) -> None:
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
        result = self.run_tool("fplinux-rotate-display", *base, *arguments)
        assert (result.returncode) == (0), result.stderr
        assert ("stub preview presented") in (result.stdout)
        holds = [line for line in result.stdout.splitlines() if "hold_us=" in line]
        assert (holds) == (expected_holds)

    @pytest.mark.parametrize(
        ("tool", "error"),
        [
            ("fplinux-jpeg", "fplinux-jpeg: input: No such file or directory\n"),
            (
                "fplinux-jpeg-cpu",
                "fplinux-jpeg-cpu: cannot read bounded regular input: missing-jpeg\n",
            ),
        ],
        ids=["fplinux-jpeg", "fplinux-jpeg-cpu"],
    )
    def test_jpeg_last_operation_and_input_reach_the_local_file_error(
        self, tool: str, error: str
    ) -> None:
        """The final decode operation accepts missing geometry and uses the last input."""
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
        assert (result.returncode) == (1)
        assert (result.stdout) == ("")
        assert (result.stderr) == (error)

    @pytest.mark.parametrize(
        ("option", "value", "error"),
        [
            pytest.param("--quality", "0", "--quality must be in 1..100", id="quality-0"),
            pytest.param("--quality", "101", "--quality must be in 1..100", id="quality-101"),
            pytest.param("--quality", "-1", "--quality must be in 1..100", id="quality--1"),
            pytest.param("--quality", "-0", "--quality must be in 1..100", id="quality--0"),
            pytest.param("--quality", "85x", "--quality must be in 1..100", id="quality-85x"),
            pytest.param(
                "--quality", "", "--quality=N requires a nonempty value", id="quality-empty"
            ),
            pytest.param("--width", "3", "encode and scale require even --width", id="width-3"),
            pytest.param("--scale", "2", "--scale 1", id="scale-2"),
            pytest.param("--scale", "4", "--scale 1", id="scale-4"),
        ],
    )
    def test_jpeg_encode_options_reject_invalid_values_before_input_access(
        self, option: str, value: str, error: str
    ) -> None:
        """Encode quality, width and scale are checked before reading input."""
        base = (
            "--operation",
            "encode",
            "--width",
            "4",
            "--height",
            "2",
            "--input",
            "missing-input",
            "--output",
            "unused-output",
        )
        result = self.assert_usage("fplinux-jpeg", *base, option, value, success=False)
        assert (error) in (result.stderr)
        self.assert_usage("fplinux-jpeg", *base, option, value, "--help", success=True)

    @pytest.mark.parametrize(
        ("operation", "geometry"),
        [("decode", ()), ("scale", ("--width", "320", "--height", "240"))],
        ids=["decode-empty", "scale-width-320-height-240"],
    )
    def test_jpeg_quality_requires_the_final_encode_operation(
        self, operation: str, geometry: tuple[str, ...]
    ) -> None:
        """Encode quality is rejected for decode and raw scaling before resources."""
        result = self.assert_usage(
            "fplinux-jpeg",
            "--operation",
            "encode",
            "--operation",
            operation,
            *geometry,
            "--input",
            "missing-input",
            "--output",
            "unused-output",
            "--quality",
            "85",
            success=False,
        )
        assert ("only encode accepts --quality") in (result.stderr)

    @pytest.mark.parametrize("divisor", ["1", "2", "4"], ids=["1", "2", "4"])
    def test_jpeg_decode_accepts_full_half_and_quarter_divisors(self, divisor: str) -> None:
        """Each supported divisor passes argument checks before the missing input error."""
        result = self.run_tool(
            "fplinux-jpeg",
            "--input",
            "missing-jpeg",
            "--output",
            "unused-output",
            "--scale",
            divisor,
        )
        assert (result.returncode) == (1), result.stderr
        assert (result.stdout) == ("")
        assert (result.stderr) == ("fplinux-jpeg: input: No such file or directory\n")

    @pytest.mark.parametrize(
        ("label", "length", "accepted"),
        [("exact", 16, True), ("short", 15, False), ("long", 17, False)],
        ids=["exact-16", "short-15", "long-17"],
    )
    def test_jpeg_tight_raw_length_and_repeated_options_are_checked_before_device_access(
        self, label: str, length: int, *, accepted: bool
    ) -> None:
        """Tight 4x2 NV16 requires sixteen bytes; repeated options use the last value."""
        missing_device = self.directory / "missing-jpeg-video"
        source = self.directory / f"jpeg-{label}.raw"
        source.write_bytes(bytes(range(length)))
        result = self.run_tool(
            "fplinux-jpeg",
            "--operation",
            "decode",
            "--operation",
            "encode",
            "--width",
            "2",
            "--width",
            "4",
            "--height",
            "4",
            "--height",
            "2",
            "--input",
            str(source),
            "--output",
            "unused-output",
            "--device",
            str(missing_device),
            "--quality",
            "1",
            "--quality",
            "100",
        )
        assert (result.returncode) == (1), result.stderr
        assert (result.stdout) == ("")
        assert (result.stderr) == (
            "fplinux-jpeg: device: No such file or directory\n"
            if accepted
            else "fplinux-jpeg: input: Invalid argument\n"
        )

    def test_present_last_mode_and_missing_drm_report_local_error(self) -> None:
        """The last mode controls output eligibility; a missing DRM device stops execution."""
        base = (
            "--input",
            "missing-input",
            "--output",
            "unused.rgb565",
            "--tty",
            str(self.directory),
            "--tty",
            "missing-tty",
            "--drm",
            "missing-drm",
        )
        result = self.run_tool("fplinux-present", *base, "--mode", "nv16", "--mode", "cpu-rgb565")
        assert (result.returncode) == (1)
        assert (result.stdout) == ("")
        assert (result.stderr) == (
            "fplinux-present: cannot open DRM display: No such file or directory\n"
        )
        self.assert_usage(
            "fplinux-present", *base, "--mode", "cpu-rgb565", "--mode", "nv16", success=False
        )
