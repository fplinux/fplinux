# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for UMS9117 shared named-resource mapping policies."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

INCLUDE = ROOT / "platforms/ums9117/linux/overlay/include"
COMPAT = ROOT / "tests/fixtures/ums9117-kernel/lcm-compat"
HARNESS = ROOT / "tests/host_tool/ums9117/shared-mmio.c"


def test_shared_mmio_mapping_preserves_resource_policies_and_errors(tmp_path: Path) -> None:
    """Check full mapping extents, exact registers and errno pointer results."""
    executable = tmp_path / "shared-mmio"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{COMPAT}",
            f"-I{INCLUDE}",
            str(HARNESS),
        ],
        name="compile UMS9117 shared MMIO mapping",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run UMS9117 shared MMIO mapping",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Shared MMIO mapping checks passed\n"
