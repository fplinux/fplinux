# SPDX-License-Identifier: GPL-2.0-only
"""Evaluate shared and board PCM configuration with a controlled card identity."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest


def test_default_pcm_uses_the_card_playback_rate_and_keeps_capture_at_48000(
    tmp_path_factory: pytest.TempPathFactory,
) -> None:
    """Use real ALSA expansion, replacing only the external control-device query."""
    source = ROOT / "tests/host_tool/sc9820e/audio"
    qin_fragment = ROOT / "targets/xiaomi-qin-1s-plus/rootfs/alsa.conf"
    with c_build_directory(tmp_path_factory, "qin_alsa_config") as directory:
        card_driver = directory / "card-driver.so"
        executable = directory / "alsa-config"
        compile_c(
            card_driver,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-shared",
                "-fPIC",
                "-DPIC",
                str(source / "card-driver.c"),
                "-lasound",
            ],
            name="compile ALSA card identity peer",
        )
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(source / "alsa-config.c"),
                "-lasound",
            ],
            name="compile real ALSA configuration evaluator",
        )
        for case, driver, fragment, expected_rate in (
            ("Qin board configuration", "SC9820E audio", qin_fragment, "44100"),
            ("Qin without board configuration", "SC9820E audio", None, "48000"),
            ("UMS9117 common configuration", "UMS9117", None, "48000"),
            ("other common configuration", "Other card", None, "48000"),
        ):
            run_process(
                [
                    str(executable),
                    str(ROOT / "alpine/aports/fplinux-audio/asound.conf"),
                    str(fragment) if fragment is not None else "-",
                    str(card_driver),
                    driver,
                    expected_rate,
                ],
                name=f"expand default PCM for {case}",
                timeout=30,
                check=True,
            )
