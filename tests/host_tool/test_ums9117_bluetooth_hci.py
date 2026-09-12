# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for UMS9117 Bluetooth setup, H4 and retained transport."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "platforms/ums9117/kernel"
FIXTURES = ROOT / "tests/host_tool"
COMPAT = FIXTURES / "bluetooth-compat"


class Ums9117BluetoothHciHostTests(unittest.TestCase):
    """Link production objects with synthetic firmware and mailbox boundaries."""

    def run_component(self, component: str) -> None:
        """Compile and execute an isolated, self-checking C fixture."""
        sources = (
            [KERNEL / "cm4-hci.c", KERNEL / "cm4-h4.c"]
            if component == "runtime"
            else [KERNEL / f"cm4-{component}.c"]
        )
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / component
            run_process(
                [
                    "cc",
                    "-O2",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-unused-parameter",
                    "-UNDEBUG",
                    f"-I{COMPAT}",
                    f"-I{KERNEL}",
                    str(FIXTURES / f"ums9117-bluetooth-{component}.c"),
                    *(str(source) for source in sources),
                    "-o",
                    str(executable),
                ],
                name=f"compile UMS9117 Bluetooth {component} host component",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name=f"run UMS9117 Bluetooth {component} host component",
                timeout=10,
                check=True,
            )

    def test_setup_wire_replies_and_firmware_admission(self) -> None:
        """Synthetic records produce exact commands and require matching replies."""
        self.run_component("setup")

    def test_h4_packets_partial_input_and_length_validation(self) -> None:
        """H4 preserves packet boundaries and rejects invalid types and lengths."""
        self.run_component("h4")

    def test_retained_transport_admission_rollback_and_stream(self) -> None:
        """Fake peer/core boundaries exercise real suspend gating and stream retention."""
        self.run_component("runtime")


if __name__ == "__main__":
    unittest.main()
