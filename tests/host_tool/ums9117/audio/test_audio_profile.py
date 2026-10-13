# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked audio-profile parsing and firmware-lifetime characterization."""

from __future__ import annotations

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

AUDIO = ROOT / "platforms/ums9117/linux/overlay/sound/drivers/ums9117"
HARNESS = ROOT / "tests/host_tool/ums9117/audio/audio-profile.c"
COMPAT = ROOT / "tests/host_tool/ums9117/audio/native-operation-compat/audio"
UNALIGNED = ROOT / "tests/fixtures/ums9117-kernel/fm-compat"


class AudioProfileHostComponentTests:
    """Replace kernel firmware/property boundaries while linking the real parser."""

    @pytest.mark.parametrize("vibrator", [0, 1], ids=["without-vibrator", "with-vibrator"])
    def test_decoding_validation_and_firmware_lifetime(
        self, vibrator: int, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Preserve packed values, unsafe-input rejection and acquired-data cleanup."""
        with c_build_directory(tmp_path_factory, "audio_profile") as temporary:
            executable = temporary / f"audio-profile-{vibrator}"
            compile_c(
                executable,
                [
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-DCONFIG_SND_UMS9117_SPEAKER_VIBRATOR={vibrator}",
                    f"-I{COMPAT}",
                    f"-I{UNALIGNED}",
                    f"-I{AUDIO}",
                    str(HARNESS),
                    str(AUDIO / "ums9117-audio-profile.c"),
                ],
                name="compile audio-profile host harness",
                check=True,
            )
            result = run_process(
                [str(executable)],
                name="run audio-profile host harness",
                timeout=30,
            )
            assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
