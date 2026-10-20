# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for UMS9117 Bluetooth setup and retained transport."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

KERNEL = ROOT / "platforms/ums9117/linux/overlay/drivers/bluetooth/ums9117"
FIXTURES = ROOT / "tests/host_tool/ums9117/radio"
KERNEL_COMPAT = ROOT / "tests/fixtures/ums9117-kernel"
COMPAT = KERNEL_COMPAT / "bluetooth-compat"
PLATFORM = ROOT / "platforms/ums9117/linux/overlay"


class Ums9117BluetoothHciHostTests:
    """Link production objects with synthetic firmware and mailbox boundaries."""

    def run_component(self, component: str, tmp_path_factory: pytest.TempPathFactory) -> None:
        """Compile and execute an isolated, self-checking C fixture."""
        source = KERNEL / "cm4-hci.c" if component == "runtime" else KERNEL / f"cm4-{component}.c"
        sources = [source]
        if component == "runtime":
            sources.append(PLATFORM / "lib/ums9117-fm-wire.c")
        flags = ["-DCONFIG_RADIO_UMS9117_CM4=1"] if component == "runtime" else []
        with c_build_directory(tmp_path_factory, "ums9117_bluetooth_hci") as temporary:
            executable = temporary / component
            compile_c(
                executable,
                [
                    "-O2",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-unused-parameter",
                    "-UNDEBUG",
                    *flags,
                    f"-I{COMPAT}",
                    f"-I{KERNEL}",
                    f"-I{PLATFORM / 'include'}",
                    str(FIXTURES / f"ums9117-bluetooth-{component}.c"),
                    *(str(source) for source in sources),
                ],
                name=f"compile UMS9117 Bluetooth {component} host component",
                check=True,
            )
            run_process(
                [str(executable)],
                name=f"run UMS9117 Bluetooth {component} host component",
                timeout=10,
                check=True,
            )

    def test_setup_wire_replies_and_firmware_admission(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Synthetic records produce exact commands and require matching replies."""
        self.run_component("setup", tmp_path_factory=tmp_path_factory)

    def test_mailbox_copies_received_values_and_retains_suspend_channel(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Host callback mutation preserves queued values; non-idle suspend is vetoed."""
        self.run_component("mailbox", tmp_path_factory=tmp_path_factory)

    def test_retained_transport_admission_rollback_and_stream(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Fake peers exercise suspend, FM framing, timeout isolation and BT retention."""
        self.run_component("runtime", tmp_path_factory=tmp_path_factory)
