# SPDX-License-Identifier: GPL-2.0-only
"""Host-compiled PCM output routing with no codec or physical audio execution."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest


def test_headphone_only_headfree_preserves_single_outputs_and_default_policy(
    tmp_path_factory: pytest.TempPathFactory,
) -> None:
    """A requested pair drops the speaker only under the explicit board policy."""
    audio = ROOT / "platforms/ums9117/linux/overlay/sound/drivers/ums9117"
    compat = ROOT / "tests/host_tool/ums9117/audio/native-operation-compat"
    with c_build_directory(tmp_path_factory, "pcm_route") as temporary:
        executable = temporary / "pcm-route"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-DCONFIG_SND_UMS9117_HEADPHONE_FED_SPEAKER=0",
                f"-I{compat / 'audio'}",
                f"-I{compat / 'codec'}",
                f"-I{audio}",
                str(ROOT / "tests/host_tool/ums9117/audio/pcm-route.c"),
            ],
            name="compile PCM output route component",
            check=True,
        )
        result = run_process([str(executable)], name="run PCM output route component", timeout=10)
        assert result.returncode == 0, result.stderr
