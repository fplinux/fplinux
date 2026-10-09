# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for the bootstrap-to-bridge handoff codec."""

from __future__ import annotations

from typing import TYPE_CHECKING, ClassVar

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

    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[None]:
        """Compile the isolated codec harness once for this test class."""
        with c_build_directory(tmp_path_factory, "fplinux-handoff-protocol") as directory:
            cls.executable = directory / "fplinux-handoff-protocol"
            compile_c(
                cls.executable,
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
            yield

    def run_case(self, case: str) -> None:
        """Run one self-checking peer case and preserve diagnostics on failure."""
        result = run_process(
            [str(self.executable), case],
            name=f"handoff protocol case {case}",
            timeout=10,
        )
        assert (result.returncode) == (0), (
            f"{case} failed:\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
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
