# SPDX-License-Identifier: GPL-2.0-only
"""Clock signal processes using production audiod, with only device boundaries faked."""

from __future__ import annotations

import os
import signal
import time
from itertools import pairwise
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.gui.clock_audio import ClockAudioService
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator

HERE = ROOT / "tests/host_tool/gui"
SESSION = ROOT / "alpine/aports/fplinux-session"
AUDIOD = ROOT / "alpine/aports/fplinux-audiod"
CARD = ROOT / "tests/host_process/gui/fixtures/audiod"


@pytest.fixture(scope="module")
def clock_signal(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the real ring client and replace only socket observation and evdev."""
    with c_build_directory(tmp_path_factory, "clock-signal") as directory:
        executable = directory / "clock-signal"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{SESSION}",
                str(SESSION / "clock-signal.c"),
                str(SESSION / "session-vibrator.c"),
                str(ROOT / "lib/fplinux/fplinux-audio-client.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                str(HERE / "clock-signal-input-boundary.c"),
                "-Wl,--wrap=open,--wrap=opendir,--wrap=connect,--wrap=ioctl,--wrap=write,--wrap=close",
                "-lm",
            ],
            name="compile Clock signal with the production audio client",
        )
        yield executable


@pytest.fixture(scope="module")
def clock_audiod(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Keep the production daemon and replace its ALSA card only."""
    with c_build_directory(tmp_path_factory, "clock-audiod") as directory:
        executable = directory / "audiod"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Os",
                "-I",
                str(CARD),
                "-I",
                str(ROOT / "include/fplinux"),
                str(AUDIOD / "audiod.c"),
                str(AUDIOD / "audio-device.c"),
                str(AUDIOD / "audio-mixer.c"),
                str(AUDIOD / "audio-streams.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                str(CARD / "boundary.c"),
                "-pthread",
            ],
            name="compile Clock signal's production audio service with fake ALSA",
        )
        yield executable


@pytest.fixture
def audio_work() -> Iterator[Path]:
    # The service's --dir and both Unix socket paths must stay short.
    with TemporaryDirectory(prefix="fpc-") as directory:
        yield Path(directory)


@pytest.fixture
def clock_audio(clock_audiod: Path, audio_work: Path) -> ClockAudioService:
    return ClockAudioService(clock_audiod, audio_work)


def audio_environment(work: Path, *, vibration: Path | None = None) -> dict[str, str]:
    input_tree = work / "input"
    input_tree.mkdir(exist_ok=True)
    environment = {
        "PATH": os.defpath,
        "FPLINUX_AUDIO_STREAM_SOCKET": str(work / "stream.sock"),
        "CLOCK_SIGNAL_INPUT_TREE": str(input_tree),
    }
    if vibration is not None:
        (input_tree / "event0").touch()
        environment["CLOCK_SIGNAL_VIBRATION_EVENTS"] = str(vibration)
    return environment


def stop_signal(process: subprocess.Popen[str], *, status: int = 0) -> None:
    process.send_signal(signal.SIGTERM)
    assert process.wait(timeout=1) == status


@pytest.mark.parametrize(
    ("arguments", "status", "stream"),
    [
        (["--help"], 0, "stdout"),
        (["--unknown", "--help"], 0, "stdout"),
        (["--unknown"], 2, "stderr"),
        (["unexpected"], 2, "stderr"),
    ],
)
def test_help_and_errors_do_not_open_devices(
    clock_signal: Path, audio_work: Path, arguments: list[str], status: int, stream: str
) -> None:
    events = audio_work / "vibration"
    result = run_process(
        [str(clock_signal), *arguments],
        name="parse Clock signal arguments without devices or audio service",
        timeout=5,
        env=audio_environment(audio_work, vibration=events),
    )
    assert result.returncode == status
    assert "clock-signal" in getattr(result, stream)
    assert "input-open" not in result.stdout
    assert "audio-connect" not in result.stdout
    assert not events.exists()


def test_unavailable_outputs_retry_without_log_flood_and_return_failure(
    clock_signal: Path, audio_work: Path
) -> None:
    def stop_after_retries(process: subprocess.Popen[str], _deadline: float) -> None:
        time.sleep(2.15)
        assert process.poll() is None, "Clock signal stopped before the service could recover"
        stop_signal(process, status=1)

    result = run_process(
        [str(clock_signal)],
        name="retry an absent Clock audio service and report no available output",
        timeout=4,
        env=audio_environment(audio_work),
        while_running=stop_after_retries,
    )
    assert result.returncode == 1
    assert result.stdout.count("input-open\n") == 1
    assert 2 <= result.stdout.count("audio-connect\n") <= 4
    assert result.stderr.count("clock-signal: audio unavailable:") == 1
    assert result.stderr.count("clock-signal: vibration unavailable:") == 1


def test_clock_tones_reach_service_pcm_alongside_app_and_dismiss_keeps_app_playing(
    clock_signal: Path, clock_audio: ClockAudioService
) -> None:
    result: subprocess.CompletedProcess[str] | None = None
    tone_start = 0

    def exercise() -> None:
        nonlocal result, tone_start
        with clock_audio.manager() as control, clock_audio.application(control) as application:
            application.refill()
            clock_audio.wait(lambda: application.field(24) >= 960, "app audio did not play")
            first_frame = len(clock_audio.played_frames())

            def dismiss_after_tones(process: subprocess.Popen[str], _deadline: float) -> None:
                nonlocal tone_start
                clock_audio.clock(control, process.pid)
                clock_audio.wait(
                    lambda: any(
                        abs(left - 500) > 500
                        for left, _ in clock_audio.played_frames()[first_frame:]
                    ),
                    "Clock signal did not join the application's PCM output",
                    tick=application.refill,
                )
                frames = clock_audio.played_frames()
                tone_start = next(
                    index
                    for index in range(first_frame, len(frames))
                    if abs(frames[index][0] - 500) > 500
                )
                clock_audio.wait(
                    lambda: len(clock_audio.played_frames()) >= tone_start + 28800,
                    "Clock signal did not produce both alert tones",
                    tick=application.refill,
                )
                stop_signal(process)
                heard = application.field(24)
                clock_audio.wait(
                    lambda: application.field(24) >= heard + 4800,
                    "dismissing the Clock signal stopped the application's audio",
                    tick=application.refill,
                )
                assert application.field(28) == 0, "the application's ring ended"
                assert all(
                    left == right == 500 for left, right in clock_audio.played_frames()[-2400:]
                )

            result = run_process(
                [str(clock_signal)],
                name="mix both Clock alert tones with an application and dismiss only the alert",
                timeout=5,
                env=audio_environment(clock_audio.work),
                while_running=dismiss_after_tones,
            )

    clock_audio.run(exercise)
    assert result is not None
    assert result.returncode == 0, result
    assert "audio unavailable" not in result.stderr
    assert "audio playback failed" not in result.stderr
    frames = clock_audio.played_frames()
    assert all(left == right for left, right in frames)
    left = [sample for sample, _ in frames]
    first = left[tone_start + 480 : tone_start + 5280]
    second = left[tone_start + 17280 : tone_start + 22080]
    assert len(first) == len(second) == 4800
    for tone, crossings in ((first, 66), (second, 88)):
        rising = sum(before <= 500 < after for before, after in pairwise(tone))
        assert abs(rising - crossings) <= 1
        assert 3490 <= max(tone) <= 3500
        assert -2500 <= min(tone) <= -2490
    assert all(sample == 500 for sample in left[tone_start + 10560 : tone_start + 15360])
    assert clock_audio.card_events().count("open default") == 1


def test_busy_service_keeps_vibration_bounded_then_plays_preserved_clock_frames(
    clock_signal: Path, clock_audio: ClockAudioService
) -> None:
    result: subprocess.CompletedProcess[str] | None = None
    vibration = clock_audio.work / "vibration"
    clock_audio.busy.touch()

    def exercise() -> None:
        nonlocal result
        with clock_audio.manager() as control:

            def release_card_after_vibration(
                process: subprocess.Popen[str], _deadline: float
            ) -> None:
                clock_audio.clock(control, process.pid)
                clock_audio.wait(
                    lambda: (
                        vibration.exists()
                        and "start\n" in vibration.read_text(encoding="ascii")
                        and clock_audio.card_events().count("busy") >= 2
                    ),
                    "the busy audio service blocked Clock vibration or its retry",
                )
                clock_audio.busy.unlink()
                clock_audio.wait(
                    lambda: len(clock_audio.played_frames()) >= 26880,
                    "Clock signal frames did not play when the card became free",
                    timeout=3,
                )
                stop_signal(process)

            result = run_process(
                [str(clock_signal)],
                name="keep Clock vibration responsive while the audio service holds a busy card",
                timeout=7,
                env=audio_environment(clock_audio.work, vibration=vibration),
                while_running=release_card_after_vibration,
            )

    clock_audio.run(exercise)
    assert result is not None
    assert result.returncode == 0, result
    assert "clock-signal: audio" not in result.stderr
    events = vibration.read_text(encoding="ascii").splitlines()
    assert events[0] == "upload 250 32767 32767"
    assert "start" in events
    assert events[-3:] == ["stop", "remove", "close"]
    frames = clock_audio.played_frames()
    assert all(left == right for left, right in frames)
    samples = [left for left, _ in frames]
    assert len(samples) >= 26880
    # 200 ms at 48 kHz, 150 ms of silence, then another 200 ms tone.
    # Exact edges also catch frames skipped when the 16384-frame ring filled.
    assert samples[0] == 0
    assert samples[1] > 0
    assert samples[9599] < 0
    assert all(sample == 0 for sample in samples[9600:16801])
    assert samples[16801] > 0
    assert samples[26399] < 0
    assert all(sample == 0 for sample in samples[26400:26880])
    for tone, crossings in (
        (samples[480:5280], 66),
        (samples[17280:22080], 88),
    ):
        rising = sum(before <= 0 < after for before, after in pairwise(tone))
        assert abs(rising - crossings) <= 1
        assert 2990 <= max(tone) <= 3000
        assert -3000 <= min(tone) <= -2990


def test_service_restart_reconnects_clock_within_the_existing_alert(
    clock_signal: Path, clock_audio: ClockAudioService
) -> None:
    result: subprocess.CompletedProcess[str] | None = None

    def first_service() -> None:
        nonlocal result
        with clock_audio.manager() as control:

            def restart_service(process: subprocess.Popen[str], _deadline: float) -> None:
                clock_audio.clock(control, process.pid)
                clock_audio.wait(
                    lambda: len(clock_audio.played_frames()) >= 1440,
                    "Clock signal did not play before the service restart",
                )
                assert clock_audio.process is not None
                clock_audio.process.terminate()
                assert clock_audio.process.wait(timeout=1) == 0
                written = len(clock_audio.played_frames())
                time.sleep(0.08)

                def resumed_service() -> None:
                    with clock_audio.manager() as resumed_control:
                        clock_audio.clock(resumed_control, process.pid)
                        clock_audio.wait(
                            lambda: len(clock_audio.played_frames()) >= written + 4800,
                            "Clock signal did not reconnect to the replacement service",
                        )
                        stop_signal(process)

                clock_audio.run(resumed_service)

            result = run_process(
                [str(clock_signal)],
                name="reconnect the running Clock signal after its audio service restarts",
                timeout=6,
                env=audio_environment(clock_audio.work),
                while_running=restart_service,
            )

    clock_audio.run(first_service)
    assert result is not None
    assert result.returncode == 0, result
    assert result.stderr.count("clock-signal: audio playback failed:") == 1
    assert result.stdout.count("audio-connect\n") >= 2
    assert clock_audio.card_events().count("open default") == 2
