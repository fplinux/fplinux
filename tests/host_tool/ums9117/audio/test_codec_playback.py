# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked codec headphone-only board policy."""

from __future__ import annotations

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process


@pytest.mark.parametrize("vibrator", [0, 1], ids=["without-vibrator", "with-vibrator"])
def test_codec_enforces_headphones_only_and_restores_its_registers(
    vibrator: int,
    tmp_path_factory: pytest.TempPathFactory,
) -> None:
    """Register storage observes fields and restoration; calibration and time are replaced."""
    audio = ROOT / "platforms/ums9117/linux/overlay/sound/drivers/ums9117"
    fixtures = ROOT / "tests/host_tool/ums9117/audio"
    with c_build_directory(tmp_path_factory, "codec_playback") as temporary:
        executable = temporary / "codec-playback"
        compile_c(
            executable,
            [
                "-O2",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-DCONFIG_SND_UMS9117_HEADPHONE_FED_SPEAKER=0",
                f"-DCONFIG_SND_UMS9117_SPEAKER_VIBRATOR={vibrator}",
                f"-I{fixtures / 'codec-playback-compat'}",
                f"-I{fixtures / 'native-operation-compat/codec'}",
                f"-I{fixtures / 'native-operation-compat/audio'}",
                f"-I{audio}",
                str(fixtures / "codec-playback.c"),
                str(audio / "ums9117-sc2720-codec.c"),
            ],
            name="compile codec playback register component",
            check=True,
        )
        result = run_process(
            [str(executable)], name="run codec playback register component", timeout=10, check=True
        )
        assert result.stdout == "codec headphone-only board policy: ok\n"
