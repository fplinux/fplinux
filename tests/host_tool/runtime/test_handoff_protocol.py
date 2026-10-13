# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for the bootstrap-to-bridge handoff codec."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

HARNESS = ROOT / "tests/host_tool/runtime/fplinux-handoff-protocol.c"


class HandoffProtocolTests:
    """Run a C99 peer for the fixed binary handoff boundary."""

    @pytest.fixture(scope="class")
    @classmethod
    def handoff_executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
        """Compile the isolated codec harness once for this test class."""
        with c_build_directory(tmp_path_factory, "fplinux-handoff-protocol") as directory:
            executable = directory / "fplinux-handoff-protocol"
            compile_c(
                executable,
                [
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(HARNESS),
                ],
                name="compile handoff protocol harness",
                check=True,
            )
            yield executable

    def run_case(self, executable: Path, case: str) -> None:
        """Run one self-checking peer case and preserve diagnostics on failure."""
        result = run_process(
            [str(executable), case],
            name=f"handoff protocol case {case}",
            timeout=10,
        )
        assert (result.returncode) == (0), (
            f"{case} failed:\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )

    def test_request_and_ack_round_trip(self, handoff_executable: Path) -> None:
        """A matching request and ACK response round-trip through the codec."""
        self.run_case(handoff_executable, "roundtrip")

    def test_tampered_payload_is_rejected(self, handoff_executable: Path) -> None:
        """Any altered request or response byte fails validation."""
        self.run_case(handoff_executable, "tamper")

    def test_verified_nack_is_not_an_ack(self, handoff_executable: Path) -> None:
        """A valid nonzero bridge status remains a rejected handoff."""
        self.run_case(handoff_executable, "nack")
