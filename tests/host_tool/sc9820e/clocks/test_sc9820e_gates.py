# SPDX-License-Identifier: GPL-2.0-only
"""Exercise clock providers with host register-map and registration doubles."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/clocks"
OVERLAY = ROOT / "platforms/sc9820e/linux/overlay"
DRIVERS = OVERLAY / "drivers/clk/sprd"


@pytest.fixture(scope="module")
def gate_component(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the provider drivers separately from the kernel API doubles."""
    with c_build_directory(tmp_path_factory, "sc9820e_gates") as temporary:
        executable = temporary / "sc9820e-gates"
        compilation = compile_c(
            executable,
            [
                "-O2",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{FIXTURES / 'gate-compat'}",
                f"-I{OVERLAY / 'include'}",
                str(FIXTURES / "sc9820e-gates.c"),
                str(DRIVERS / "sc9820e-gate.c"),
                str(DRIVERS / "sc9820e-peripheral-gates.c"),
                str(DRIVERS / "sc9820e-sdio-clock.c"),
            ],
            name="compile SC9820E clock provider host component",
            check=False,
        )
        assert compilation.returncode == 0, compilation.stdout + compilation.stderr
        yield executable


@pytest.mark.parametrize(
    "provider", ["hwspinlock", "hs-spi", "gpio", "ap-dma", "efuse", "mailbox", "sdio"]
)
def test_registered_gate_callbacks_preserve_registers_and_errors(
    gate_component: Path, provider: str
) -> None:
    """Probe each provider and exercise its exported gate, without a running kernel."""
    result = run_process(
        [str(gate_component), provider],
        name=f"run SC9820E {provider} clock provider host component",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
