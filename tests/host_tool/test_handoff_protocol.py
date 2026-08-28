# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for the bootstrap-to-bridge handoff codec."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tests/host_tool/fplinux-handoff-protocol.c"


class HandoffProtocolTests(unittest.TestCase):
    """Run a C99 peer for the fixed binary handoff boundary."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Compile the isolated codec harness once for this test class."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.executable = Path(cls.temporary.name) / "fplinux-handoff-protocol"
        run_process(
            [
                "cc",
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(HARNESS),
                "-o",
                str(cls.executable),
            ],
            name="compile handoff protocol harness",
            timeout=30,
            check=True,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        """Remove the compiled host harness."""
        cls.temporary.cleanup()

    def run_case(self, case: str) -> None:
        """Run one self-checking peer case and preserve diagnostics on failure."""
        result = run_process(
            [str(self.executable), case],
            name=f"handoff protocol case {case}",
            timeout=10,
        )
        self.assertEqual(
            result.returncode,
            0,
            msg=f"{case} failed:\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )

    def test_request_and_ack_round_trip(self) -> None:
        """A matching request and ACK response round-trip through the codec."""
        self.run_case("roundtrip")

    def test_tampered_payload_is_rejected(self) -> None:
        """Any altered request or response byte fails validation."""
        self.run_case("tamper")

    def test_verified_nack_is_not_an_ack(self) -> None:
        """A valid nonzero bridge status remains a rejected handoff."""
        self.run_case("nack")


if __name__ == "__main__":
    unittest.main()
