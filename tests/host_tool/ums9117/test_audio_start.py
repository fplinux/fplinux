# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked DAC start register protocol with controlled kernel services."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

AUDIO = ROOT / "platforms/ums9117/linux/sound/drivers/ums9117"
HARNESS = ROOT / "tests/host_tool/ums9117/audio-start.c"
COMPAT = ROOT / "tests/host_tool/ums9117/native-operation-compat/vbc"
TYPES = ROOT / "tests/host_tool/ums9117/native-operation-compat/audio"


class AudioStartHostComponentTests:
    """Observe register transactions from the production DAC lifecycle."""

    def test_clocked_alc_start_preserves_live_and_disabled_states(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Clear enabled ALC with active DAC lanes and retain selected processing.

        The MMIO boundary stores register words and observes clear transactions;
        it does not implement DSP processing or an analog audio output.
        """
        with c_build_directory(tmp_path_factory, "audio_start") as temporary:
            executable = temporary / "audio-start"
            compile_c(
                executable,
                [
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-O2",
                    "-DCONFIG_SND_UMS9117_SPEAKER_VIBRATOR=0",
                    f"-I{COMPAT}",
                    f"-I{TYPES}",
                    f"-I{AUDIO}",
                    str(HARNESS),
                    str(AUDIO / "ums9117-audio.c"),
                ],
                name="compile audio-start host register fixture",
                check=True,
            )
            result = run_process(
                [str(executable)],
                name="run audio-start host register fixture",
                timeout=30,
            )
            assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
