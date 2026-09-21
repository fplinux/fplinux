# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for the independently testable image-rotation contract."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
APORT = ROOT / "alpine/aports/fplinux-rotate"
SHARED = ROOT / "include/fplinux"
HARNESS = ROOT / "tests/host_tool/fplinux-rotate-core.c"


class FplinuxRotateHostToolTests(unittest.TestCase):
    """Exercise real production objects without claiming V4L2 hardware coverage."""

    def _compile(self, output: Path, *sources: Path) -> None:
        run_process(
            [
                "cc",
                "-O2",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{APORT}",
                f"-I{SHARED}",
                *(str(source) for source in sources),
                "-o",
                str(output),
            ],
            name="compile FPLinux rotation host executable",
            timeout=30,
            check=True,
        )

    def test_cpu_reference_against_independent_oracle(self) -> None:
        """Protect crop/stride/chroma/guard/source invariants for every mode."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "fplinux-rotate-core-test"
            self._compile(executable, HARNESS, APORT / "fplinux-rotate-core.c")
            run_process(
                [str(executable)],
                name="run FPLinux rotation CPU oracle",
                timeout=30,
                check=True,
            )

    def test_cpu_cli_writes_ram_result_and_stable_measurement(self) -> None:
        """Protect the installable CLI's raw result and benchmark interface."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = directory / "fplinux-rotate"
            output = directory / "result.raw"
            self._compile(
                executable,
                APORT / "fplinux-rotate.c",
                APORT / "fplinux-rotate-core.c",
                ROOT / "lib/fplinux/fplinux-fb-session.c",
                ROOT / "lib/fplinux/fplinux-cli.c",
            )
            result = run_process(
                [
                    str(executable),
                    "--engine",
                    "cpu",
                    "--format",
                    "rgb565",
                    "--width",
                    "320",
                    "--height",
                    "240",
                    "--rotate",
                    "90",
                    "--verify",
                    "--iterations",
                    "2",
                    "--output",
                    str(output),
                ],
                name="run FPLinux rotation CPU CLI",
                timeout=30,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("benchmark stage=selected engine=cpu iterations=2", result.stdout)
            self.assertIn("process_user_us=", result.stdout)
            self.assertEqual(output.stat().st_size, 240 * 320 * 2)


if __name__ == "__main__":
    unittest.main()
