# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked audio-profile parsing and firmware-lifetime characterization."""

from __future__ import annotations

import tempfile
from pathlib import Path

import pytest

from tests import ROOT
from tests.process import run_process

AUDIO = ROOT / "platforms/ums9117/linux/sound/drivers/ums9117"
HARNESS = ROOT / "tests/host_tool/ums9117/audio-profile.c"
COMPAT = ROOT / "tests/host_tool/ums9117/native-operation-compat/audio"
UNALIGNED = ROOT / "tests/host_tool/ums9117/fm-compat"


class AudioProfileHostComponentTests:
    """Replace kernel firmware/property boundaries while linking the real parser."""

    @pytest.mark.parametrize("vibrator", [0, 1], ids=["without-vibrator", "with-vibrator"])
    def test_decoding_validation_and_firmware_lifetime(self, vibrator: int) -> None:
        """Preserve packed values, unsafe-input rejection and acquired-data cleanup."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / f"audio-profile-{vibrator}"
            run_process(
                [
                    "cc",
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
                    "-o",
                    str(executable),
                ],
                name="compile audio-profile host harness",
                timeout=30,
                check=True,
            )
            result = run_process(
                [str(executable)],
                name="run audio-profile host harness",
                timeout=30,
            )
            assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
