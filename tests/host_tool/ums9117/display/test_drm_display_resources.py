# SPDX-License-Identifier: GPL-2.0-only
"""Link display initialization to controlled host MMIO and subsystem boundaries."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

DISPLAY = ROOT / "platforms/ums9117/linux/overlay/drivers/gpu/drm/ums9117"
FIXTURES = ROOT / "tests/host_tool/ums9117/display"
COMPAT = FIXTURES / "display-compat"
LCM_COMPAT = ROOT / "tests/fixtures/ums9117-kernel/lcm-compat"


@pytest.fixture(scope="module")
def display_resources(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Compile the complete production display file as a separate translation unit."""
    with c_build_directory(tmp_path_factory, "drm-display-resources") as temporary:
        executable = temporary / "drm-display-resources"
        compile_c(
            executable,
            [
                "-O2",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-Wno-sign-compare",
                "-ffunction-sections",
                "-fdata-sections",
                f"-I{COMPAT}",
                f"-I{LCM_COMPAT}",
                f"-I{DISPLAY}",
                f"-I{ROOT / 'platforms/ums9117/linux/overlay/include'}",
                "-include",
                str(COMPAT / "display-test-kernel.h"),
                str(FIXTURES / "drm-display-resources.c"),
                str(DISPLAY / "ums9117-drm-display.c"),
                "-Wl,--gc-sections",
            ],
            name="link UMS9117 display initialization with host subsystem boundaries",
            check=True,
        )
        yield executable


@pytest.mark.parametrize(
    "resource",
    ["success", "ap-ahb-gate-set", "ap-ahb-reset-set", "ap-ahb-reset-clear"],
)
def test_display_initialization_preserves_mapping_errors(
    display_resources: Path, resource: str
) -> None:
    """A shared mapping failure returns ENOMEM before hardware initialization."""
    result = run_process(
        [str(display_resources), resource],
        name=f"exercise linked UMS9117 display initialization: {resource}",
        timeout=10,
        check=True,
    )
    assert result.stdout == "DRM display resource checks passed\n"
