# SPDX-License-Identifier: GPL-2.0-only
"""Run the kernel-log forwarder against a pipe replacing the kernel device."""

from __future__ import annotations

import os
import subprocess
import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


class KernelLogForwarderTests:
    """Check output records and CLI behavior, not kernel log ingestion."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link production code with a fake kernel sink at the open boundary."""
        with ExitStack() as cleanup:
            cls.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(cls.temporary)
            cls.executable = Path(cls.temporary.name) / "fplinux-kmsg"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include/fplinux"),
                    str(ROOT / "alpine/aports/fplinux-base/fplinux-kmsg.c"),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    str(ROOT / "tests/host_tool/runtime/fplinux-kmsg-sink.c"),
                    "-Wl,--wrap=open",
                    "-o",
                    str(cls.executable),
                ],
                name="compile kernel-log forwarder",
                timeout=30,
                check=True,
            )
            yield

    @pytest.mark.parametrize("level", [3, 4, 6], ids=["3", "4", "6"])
    def test_lines_use_requested_priority_and_one_component_prefix(self, level: int) -> None:
        """Preserve message content, including an unterminated last line."""
        result = subprocess.run(
            [str(self.executable), "--level", str(level), "--tag", "fplinux-input"],
            capture_output=True,
            check=False,
            text=True,
            input="ready\nfplinux-input: failed\nlast",
            timeout=5,
        )
        assert (result.returncode) == (0), result.stderr
        assert (result.stderr) == ("")
        assert (result.stdout) == (
            f"<{level}>fplinux-input: ready\n<{level}>fplinux-input: failed\n"
            f"<{level}>fplinux-input: last\n"
        )

    def test_long_line_is_preserved_across_bounded_records(self) -> None:
        """No input bytes are lost when a daemon line exceeds the kernel record size."""
        body = "x" * 4000
        result = subprocess.run(
            [str(self.executable), "--level", "6", "--tag", "daemon"],
            capture_output=True,
            check=False,
            text=True,
            input=body + "\n",
            timeout=5,
        )
        assert (result.returncode) == (0), result.stderr
        records = result.stdout.splitlines(keepends=True)
        assert (len(records)) > (1)
        assert all(len(record.encode()) <= 1024 for record in records)
        assert all(record.startswith("<6>daemon: ") for record in records)
        assert (
            "".join(record.removeprefix("<6>daemon: ").removesuffix("\n") for record in records)
        ) == (body)

    @pytest.mark.parametrize(
        ("arguments", "status"),
        [(["--help"], 0), (["--level", "8", "--tag", "test"], 2)],
        ids=["help", "level-8-tag-test"],
    )
    def test_help_and_invalid_options_precede_device_access(
        self, arguments: list[str], status: int
    ) -> None:
        """Help and usage errors do not try to open the denied kernel sink."""
        environment = {**os.environ, "FPLINUX_TEST_KMSG_DENY_OPEN": "1"}
        result = subprocess.run(
            [str(self.executable), *arguments],
            capture_output=True,
            check=False,
            text=True,
            env=environment,
            timeout=5,
        )
        assert (result.returncode) == (status), result.stderr
        assert ("cannot open") not in (result.stderr)
        result = subprocess.run(
            [str(self.executable), "--level", "3", "--tag", "test"],
            capture_output=True,
            check=False,
            text=True,
            env=environment,
            timeout=5,
        )
        assert (result.returncode) == (1)
        assert ("fplinux-kmsg: cannot open /dev/kmsg:") in (result.stderr)
