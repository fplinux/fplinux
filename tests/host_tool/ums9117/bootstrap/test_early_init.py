# SPDX-License-Identifier: GPL-2.0-only
"""Host MMIO effects of the target-owned bootstrap initialization."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.display import render_bootstrap_display
from fplinux_cli.manifests.display import load_display

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize(
    ("target", "foundation", "radio", "camera", "xtl0", "analog_reset", "panel"),
    [
        (
            "nokia-ta1618",
            "0 8208a 8388a 18208a",
            "8c004 30 82044",
            "a5a5a5a5 a5a5a5a5 a5a5a5a5 a5a5a5a5",
            "142",
            True,
            "0 182001 0 a5a5a5a5",
        ),
        (
            "inoi-240-modern-4g",
            "30 82045 8388a 18208a",
            "8c004 10 182004",
            "a5a5a5a5 a5a5a5a5 182001 82001",
            "142",
            True,
            "0 182001 10 10",
        ),
        (
            "inoi-244-modern-4g",
            "30 82045 82002 18208a",
            "8c004 10 182004",
            "a5a5a5a5 a5a5a5a5 182001 82001",
            "142",
            False,
            "0 182001 10 10",
        ),
        (
            "maxvi-k15n-4g",
            "0 8208a 8388a 18208a",
            "8c004 a5a5a5a5 a5a5a5a5",
            "a5a5a5a5 a5a5a5a5 a5a5a5a5 a5a5a5a5",
            "142",
            True,
            "0 182001 10 20",
        ),
        (
            "bq-2830l-tank-connect-max",
            "30 102001 8388a 10208a",
            "8c004 30 102001",
            "a5a5a5a5 102000 a5a5a5a5 a5a5a5a5",
            "146",
            True,
            "0 102001 0 a5a5a5a5",
        ),
        (
            "nokia-110-4g",
            "0 8208a 8388a 18208a",
            "8c004 30 82044",
            "182000 182000 a5a5a5a5 a5a5a5a5",
            "142",
            True,
            "0 182001 0 a5a5a5a5",
        ),
        (
            "uniwa-v1000-4g",
            "0 8208a 82002 18208a",
            "8c004 30 82044",
            "a5a5a5a5 182000 a5a5a5a5 a5a5a5a5",
            "142",
            False,
            "0 182001 0 a5a5a5a5",
        ),
    ],
)
def test_target_early_init_preserves_foundation_radio_and_panel_effects(  # noqa: PLR0913 -- independent board expectations remain explicit.
    tmp_path: Path,
    target: str,
    *,
    foundation: str,
    radio: str,
    camera: str,
    xtl0: str,
    analog_reset: bool,
    panel: str,
) -> None:
    display = load_display(target, {"panel": "display/panel.toml", "boot_font": "6x8"})
    panel_data = render_bootstrap_display(display)
    assert panel_data is not None
    (tmp_path / "fplinux-panel-data.h").write_bytes(panel_data)
    executable = tmp_path / "early-init"
    arguments = [
        "-std=c99",
        "-O2",
        "-pedantic",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{tmp_path}",
        f"-I{ROOT / 'platforms/ums9117/bootstrap/src'}",
        f"-I{ROOT / 'targets' / target / 'bootstrap'}",
    ]
    if target == "nokia-110-4g":
        arguments.append("-DFPLINUX_TEST_SPI_SEND=1")
    compile_c(
        executable,
        [*arguments, str(ROOT / "tests/host_tool/ums9117/bootstrap/early-init.c")],
        name="compile target bootstrap early-init effects",
    )

    result = run_process(
        [str(executable)],
        name="observe target bootstrap early-init effects",
        timeout=5,
        check=True,
    )

    analog = f"analog 4060860c:{xtl0} 40608610:111 40608614:142 40608618:142 40608600:140"
    if analog_reset:
        analog += " 40608640:301"
    nand_data6 = "102000" if target == "bq-2830l-tank-connect-max" else "182000"
    assert result.stdout == (
        f"foundation {foundation}\nnand-data6 {nand_data6}\n"
        f"radio {radio}\ncamera {camera}\n{analog}\npanel {panel}\n"
    )
