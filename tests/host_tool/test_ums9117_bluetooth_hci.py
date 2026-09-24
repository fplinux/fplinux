# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for UMS9117 Bluetooth setup and retained transport."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "platforms/ums9117/linux/drivers/bluetooth/ums9117"
FIXTURES = ROOT / "tests/host_tool"
COMPAT = FIXTURES / "bluetooth-compat"


class Ums9117BluetoothHciHostTests(unittest.TestCase):
    """Link production objects with synthetic firmware and mailbox boundaries."""

    def run_component(self, component: str) -> None:
        """Compile and execute an isolated, self-checking C fixture."""
        source = KERNEL / "cm4-hci.c" if component == "runtime" else KERNEL / f"cm4-{component}.c"
        sources = [source]
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

    def test_mailbox_copies_received_values_and_retains_suspend_channel(self) -> None:
        """Host callback mutation preserves queued values; non-idle suspend is vetoed."""
        self.run_component("mailbox")

    def test_retained_transport_admission_rollback_and_stream(self) -> None:
        """Fake peers exercise suspend, FM framing, timeout isolation and BT retention."""
        self.run_component("runtime")


if __name__ == "__main__":
    unittest.main()
