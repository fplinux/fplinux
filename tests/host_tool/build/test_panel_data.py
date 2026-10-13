# SPDX-License-Identifier: GPL-2.0-only
"""Host-compiled generated tables with literal protocol expectations."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build.device_tree import exact_path_properties
from fplinux_cli.build.display import render_bootstrap_display, render_linux_display_files

from tests import ROOT
from tests.process import run_process
from tests.small.build.test_display import (
    panel_profile,  # noqa: F401 -- pytest fixture discovery.
    panel_root,  # noqa: F401 -- pytest fixture dependency.
)

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize(
    ("control_base", "data_base", "expected_pads", "expected_addresses"),
    [
        (
            0xB0,
            0x1F8,
            "b0:10 4b0:182004 b4:30 4b4:82046 1f8:10 5f8:182004 \n",
            "402a00b0 402a04b0 402a01f8 402a05f8\n",
        ),
        (
            0xD0,
            0x208,
            "d0:10 4d0:182004 d4:30 4d4:82046 208:10 608:182004 \n",
            "402a00d0 402a04d0 402a0208 402a0608\n",
        ),
    ],
)
def test_compiled_panel_data_preserves_sequences_and_projects_pin_base_changes(  # noqa: PLR0913 -- literal input and both consumer expectations stay together.
    tmp_path: Path,
    panel_display: dict[str, Any],
    *,
    control_base: int,
    data_base: int,
    expected_pads: str,
    expected_addresses: str,
) -> None:
    panel_display["panel"]["pins"]["control_base"] = control_base
    panel_display["panel"]["pins"]["data_base"] = data_base
    bootstrap_data = render_bootstrap_display(panel_display)
    assert bootstrap_data is not None
    (tmp_path / "fplinux-panel-data.h").write_bytes(bootstrap_data)
    for name, contents in render_linux_display_files("demo", panel_display).items():
        (tmp_path / name.rsplit("/", 1)[-1]).write_bytes(contents)
    results: dict[str, str] = {}
    for consumer in ("bootstrap", "linux"):
        executable = tmp_path / consumer
        run_process(
            [
                "cc",
                "-std=gnu99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(tmp_path),
                "-I",
                str(ROOT / "platforms/ums9117/linux/drivers/gpu/drm/ums9117"),
                "-I",
                str(ROOT / "tests/host_tool/ums9117/lcm-compat"),
                str(ROOT / f"tests/host_tool/build/fixtures/panel-{consumer}.c"),
                "-o",
                str(executable),
            ],
            name=f"compile generated {consumer} panel data",
            timeout=15,
            check=True,
        )
        result = run_process(
            [str(executable)], name=f"read compiled {consumer} panel data", timeout=5, check=True
        )
        results[consumer] = result.stdout
    assert results["bootstrap"] == "4660 65535 128 160 72\n813a5580114078802900\n" + expected_pads
    assert results["linux"] == (
        "128 160\n"
        + expected_addresses
        + "3a/1/0/55 c6/1/0/18 \n11/0/120/ 29/0/0/ \n36/1/0/48 11/0/200/ \n"
    )


def test_compiled_panel_properties_keep_offsets_and_values_paired(
    tmp_path: Path, panel_display: dict[str, Any]
) -> None:
    for name, contents in render_linux_display_files("demo", panel_display).items():
        (tmp_path / name.rsplit("/", 1)[-1]).write_bytes(contents)
    source = tmp_path / "panel.dts"
    source.write_bytes((ROOT / "tests/host_tool/build/fixtures/panel.dts.fixture").read_bytes())
    dtb = tmp_path / "panel.dtb"
    run_process(
        ["dtc", "-I", "dts", "-O", "dtb", "-i", str(tmp_path), "-o", str(dtb), str(source)],
        name="compile generated panel properties",
        timeout=15,
        check=True,
    )
    properties = exact_path_properties(dtb, "/panel")["/panel"]
    cells = {
        name: [cell for (cell,) in struct.iter_unpack(">I", value)]
        for name, value in properties.items()
    }
    assert cells == {
        "sprd,dbi-timing-ns": [50, 55, 110, 20, 70, 35],
        "sprd,pinconf-data-offsets": [0],
        "sprd,pinconf-data-values": [0x182004],
        "sprd,pinmux-data-offsets": [0],
        "sprd,pinmux-data-values": [0x10],
        "sprd,pinconf-offsets": [0, 4],
        "sprd,pinconf-values": [0x182004, 0x82046],
        "sprd,pinmux-offsets": [0, 4],
        "sprd,pinmux-values": [0x10, 0x30],
    }
