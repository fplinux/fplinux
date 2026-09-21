# SPDX-License-Identifier: GPL-2.0-only
"""Run the kernel-log forwarder against a pipe replacing the kernel device."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]


class KernelLogForwarderTests(unittest.TestCase):
    """Check output records and CLI behavior, not kernel log ingestion."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Link production code with a fake kernel sink at the open boundary."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
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
                str(ROOT / "tests/host_tool/fplinux-kmsg-sink.c"),
                "-Wl,--wrap=open",
                "-o",
                str(cls.executable),
            ],
            name="compile kernel-log forwarder",
            timeout=30,
            check=True,
        )

    def test_lines_use_requested_priority_and_one_component_prefix(self) -> None:
        """Preserve message content, including an unterminated last line."""
        for level in (3, 4, 6):
            with self.subTest(level=level):
                result = subprocess.run(
                    [str(self.executable), "--level", str(level), "--tag", "fplinux-input"],
                    capture_output=True,
                    check=False,
                    text=True,
                    input="ready\nfplinux-input: failed\nlast",
                    timeout=5,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertEqual(
                    result.stdout,
                    f"<{level}>fplinux-input: ready\n<{level}>fplinux-input: failed\n"
                    f"<{level}>fplinux-input: last\n",
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
        self.assertEqual(result.returncode, 0, result.stderr)
        records = result.stdout.splitlines(keepends=True)
        self.assertGreater(len(records), 1)
        self.assertTrue(all(len(record.encode()) <= 1024 for record in records))
        self.assertTrue(all(record.startswith("<6>daemon: ") for record in records))
        self.assertEqual(
            "".join(record.removeprefix("<6>daemon: ").removesuffix("\n") for record in records),
            body,
        )

    def test_help_and_invalid_options_precede_device_access(self) -> None:
        """Help and usage errors do not try to open the denied kernel sink."""
        environment = {**os.environ, "FPLINUX_TEST_KMSG_DENY_OPEN": "1"}
        for arguments, status in ((["--help"], 0), (["--level", "8", "--tag", "test"], 2)):
            with self.subTest(arguments=arguments):
                result = subprocess.run(
                    [str(self.executable), *arguments],
                    capture_output=True,
                    check=False,
                    text=True,
                    env=environment,
                    timeout=5,
                )
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertNotIn("cannot open", result.stderr)
        result = subprocess.run(
            [str(self.executable), "--level", "3", "--tag", "test"],
            capture_output=True,
            check=False,
            text=True,
            env=environment,
            timeout=5,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("fplinux-kmsg: cannot open /dev/kmsg:", result.stderr)


if __name__ == "__main__":
    unittest.main()
