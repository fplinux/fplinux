# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for isolated MicroPythonOS launcher helpers."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tests/fplinux-micropythonos-launcher-helpers.c"
LAUNCHER = ROOT / "alpine/aports/fplinux-micropythonos/fplinux-micropythonos-launcher.c"


class MicroPythonOsLauncherHostHelperTests(unittest.TestCase):
    """Exercise C helpers on the host, not a framebuffer, VT, or phone."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Compile the host harness once for this test class."""
        cls.temporary = tempfile.TemporaryDirectory()
        root = Path(cls.temporary.name)
        cls.executable = root / "fplinux-micropythonos-launcher"
        launcher_object = root / "fplinux-micropythonos-launcher.o"
        lock_path = root / "session.lock"
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Dmain=fplinux_micropythonos_launcher_program_main",
                f'-DFPLINUX_MICROPYTHONOS_LAUNCHER_LOCK_PATH="{lock_path}"',
                "-c",
                str(LAUNCHER),
                "-o",
                str(launcher_object),
            ],
            check=True,
        )
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(HARNESS),
                str(launcher_object),
                "-o",
                str(cls.executable),
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        """Remove the compiled host harness and its lock file."""
        cls.temporary.cleanup()

    def run_harness_case(self, case: str) -> None:
        """Run one self-checking C harness case and report its diagnostics."""
        result = subprocess.run(
            [str(self.executable), case],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(
            result.returncode,
            0,
            msg=f"{case} failed:\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )

    def test_host_display_metadata_predicates(self) -> None:
        """Host-only: accept supported metadata and reject three pages."""
        self.run_harness_case("framebuffer")

    def test_host_child_launch_helpers(self) -> None:
        """Host-only: preserve a child argument and its exit status."""
        self.run_harness_case("command")

    def test_host_session_lock_helper(self) -> None:
        """Host-only: a held fcntl lock rejects a peer process."""
        self.run_harness_case("lock")


if __name__ == "__main__":
    unittest.main()
