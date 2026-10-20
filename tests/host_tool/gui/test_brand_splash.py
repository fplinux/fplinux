# SPDX-License-Identifier: GPL-2.0-only
"""Generate opaque compositor RGB565 marks from SVG masters and theme colours."""

from __future__ import annotations

import os
import shutil
import struct
import sys
from pathlib import Path

import pytest

from tests import ROOT
from tests.process import run_process


@pytest.fixture(scope="module")
def splash_resources(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Run the actual brand generator with controlled masters and theme colours."""
    directory = tmp_path_factory.mktemp("brand-splash")
    fixtures = Path(__file__).with_name("brand-fixtures")
    shutil.copyfile(
        ROOT / "alpine/aports/fplinux-brand/generate-brand.py", directory / "generate-brand.py"
    )
    shutil.copyfile(ROOT / "alpine/aports/fplinux-icons/svgraster.py", directory / "svgraster.py")
    for kind in ("mark", "lockup"):
        for variant in ("light", "dark"):
            shutil.copyfile(fixtures / "mark.svg", directory / f"fplinux-{kind}-{variant}.svg")
    output = directory / "brand"
    run_process(
        [
            sys.executable,
            str(directory / "generate-brand.py"),
            "--profile",
            str(fixtures / "profile.json"),
            "--out",
            str(output),
        ],
        name="generate opaque compositor brand resources",
        env={**os.environ, "NODE_PATH": "/opt/quality/node-tools/node_modules"},
        timeout=30,
        check=True,
    )
    return output


@pytest.mark.parametrize(
    ("variant", "background", "blend"), [("light", 0xFFFF, 0x7BFF), ("dark", 0x0000, 0x0010)]
)
def test_splash_pixels_use_the_master_alpha_and_profile_background(
    splash_resources: Path, variant: str, background: int, blend: int
) -> None:
    """Opaque red, half-alpha blue and clear SVG regions keep distinct literal pixels."""
    data = (splash_resources / f"splash-{variant}-32.rgb565").read_bytes()
    assert len(data) == 2048
    pixels = struct.unpack("<1024H", data)
    assert pixels[1 * 32 + 1] == 0xF800
    assert pixels[1 * 32 + 30] == blend
    assert pixels[30 * 32 + 30] == background
