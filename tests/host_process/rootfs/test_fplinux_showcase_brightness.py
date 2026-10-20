# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for Showcase's brightness lease and borrowed LED state."""

from __future__ import annotations

import os
import re
import shlex
import socket
import tempfile
import threading
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

if TYPE_CHECKING:
    from collections.abc import Iterator

from tests import ROOT
from tests.fixtures.psf_font import write_solid_ascii_font
from tests.process import run_process

APORT = ROOT / "alpine/aports/fplinux-showcase"
SHARED = ROOT / "include/fplinux"
FAKE_DEVICES = ROOT / "tests/host_process/rootfs/fplinux-showcase-fake-devices.c"


@dataclass(frozen=True)
class ShowcaseScene:
    """One scene's mutable device paths and the immutable compiled command."""

    executable: Path
    work: Path

    def run_showcase(  # noqa: PLR0913 -- independent scene and input controls stay explicit.
        self,
        *,
        extra_frame: bool = False,
        vt_cycle: bool = False,
        font_error: str | None = None,
        led_trigger: str = "input-events",
        led_brightness: int = 1,
        right_soft_key: str | None = None,
        renderer: str | None = "software",
        gpu_init_failure: bool = False,
    ) -> list[str]:
        """Collect requests sent over a test-owned Unix socket during a scene."""
        case = Path(tempfile.mkdtemp(dir=self.work, prefix="scene-"))
        keypad_led = case / "keypad"
        keypad_led.mkdir()
        (keypad_led / "brightness").write_text(f"{led_brightness}\n", encoding="ascii")
        (keypad_led / "max_brightness").write_text("1\n", encoding="ascii")
        trigger_text = (
            "none [input-events] timer\n"
            if led_trigger == "input-events"
            else "[none] input-events timer\n"
        )
        (keypad_led / "trigger").write_text(trigger_text, encoding="ascii")
        socket_path = case / "brightness.sock"
        requests: list[str] = []
        server_errors: list[Exception] = []
        stopping = threading.Event()

        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as server:
            server.bind(str(socket_path))
            server.listen(1)
            server.settimeout(5)

            def serve() -> None:
                try:
                    connection, _ = server.accept()
                    with connection:
                        if stopping.is_set():
                            return
                        connection.settimeout(5)
                        while request := connection.recv(32):
                            requests.append(request.decode("ascii"))
                            connection.sendall(b"OK")
                except (OSError, UnicodeError) as error:
                    server_errors.append(error)

            worker = threading.Thread(target=serve, daemon=True)
            worker.start()
            try:
                environment = os.environ.copy()
                environment.pop("SHOWCASE_GPU_INIT_FAIL", None)
                environment["SHOWCASE_BRIGHTNESS_SOCKET"] = str(socket_path)
                environment["SHOWCASE_KEYPAD_LED"] = str(keypad_led)
                font_path = case / "font.psf"
                if font_error == "malformed":
                    font_path.write_bytes(b"not a PSF font")
                elif font_error == "wrong-size":
                    write_solid_ascii_font(font_path, width=8, height=16)
                elif font_error != "missing":
                    write_solid_ascii_font(font_path, width=6, height=12)
                environment["SHOWCASE_FONT_PATH"] = str(font_path)
                if right_soft_key is not None:
                    environment["SHOWCASE_RIGHT_SOFT_KEY"] = right_soft_key
                if extra_frame or vt_cycle:
                    environment["SHOWCASE_EXTRA_FRAME"] = "1"
                if vt_cycle:
                    environment["SHOWCASE_VT_CYCLE"] = "1"
                if gpu_init_failure:
                    environment["SHOWCASE_GPU_INIT_FAIL"] = "1"
                arguments = [str(self.executable), "--keypad-led", str(keypad_led)]
                if renderer is not None:
                    arguments.extend(["--renderer", renderer])
                result = run_process(
                    arguments,
                    name="render Showcase with a local brightness service",
                    timeout=10,
                    env=environment,
                )
            finally:
                stopping.set()
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as wakeup:
                    wakeup.connect(str(socket_path))
                worker.join(timeout=5)

        if font_error:
            assert (result.returncode) == (1), result.stderr
            if font_error == "wrong-size":
                assert ("default font is 8x16; 128-pixel display requires 6x12") in (result.stderr)
            else:
                assert ("cannot load font /usr/share/fplinux/fonts/default.psf") in (result.stderr)
            assert (result.stdout) == ("")
        elif gpu_init_failure and renderer == "gpu":
            assert result.returncode == 1, result.stderr
            assert "controlled GPU initialization failure" in result.stderr
            assert result.stdout == ""
        else:
            assert (result.returncode) == (0), result.stderr
            if gpu_init_failure:
                assert "controlled GPU initialization failure" in result.stderr
        assert not (worker.is_alive()), "brightness service did not stop"
        assert (server_errors) == ([])
        assert ((keypad_led / "brightness").read_text(encoding="ascii").splitlines()[0]) == (
            str(led_brightness)
        )
        selected = re.findall(r"\[([^\]]+)\]", (keypad_led / "trigger").read_text("ascii"))
        assert (selected) == ([led_trigger])
        return requests


@pytest.fixture(scope="class")
def executable() -> Iterator[Path]:
    """Compile the real Showcase command with simulated device boundaries."""
    with ExitStack() as cleanup:
        temporary = tempfile.TemporaryDirectory()
        cleanup.enter_context(temporary)
        executable = Path(temporary.name) / "fplinux-showcase"
        drm_flags = shlex.split(
            run_process(
                ["pkg-config", "--cflags", "--libs", "libdrm"],
                name="read DRM compiler and linker flags",
                timeout=10,
                check=True,
            ).stdout
        )
        run_process(
            [
                "cc",
                "-O2",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-isystem",
                "/opt/quality/include",
                f"-I{APORT}",
                f"-I{SHARED}",
                str(APORT / "fplinux-showcase.c"),
                str(APORT / "showcase-hardware.c"),
                str(APORT / "armada-scene.c"),
                str(APORT / "armada-storyboard.c"),
                str(APORT / "armada-renderer.c"),
                str(APORT / "armada-geometry.c"),
                str(APORT / "armada-overlay.c"),
                str(APORT / "armada-gpu-renderer.c"),
                str(ROOT / "lib/fplinux/fplinux-font.c"),
                str(ROOT / "lib/fplinux/fplinux-gpu.c"),
                str(ROOT / "lib/fplinux/fplinux-keypad-light.c"),
                str(ROOT / "lib/fplinux/fplinux-brightness-client.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                str(FAKE_DEVICES),
                "-Wl,--wrap=open",
                "-Wl,--wrap=fopen",
                "-Wl,--wrap=ioctl",
                "-Wl,--wrap=read",
                "-Wl,--wrap=write",
                "-Wl,--wrap=connect",
                "-Wl,--wrap=close",
                "-ldl",
                *drm_flags,
                "-o",
                str(executable),
            ],
            name="compile Showcase against controlled device boundaries",
            timeout=30,
            check=True,
        )
        yield executable


@pytest.fixture
def scene(executable: Path) -> Iterator[ShowcaseScene]:
    """Give each scene its own LED, font and Unix socket paths."""
    with ExitStack() as cleanup:
        case = tempfile.TemporaryDirectory()
        cleanup.enter_context(case)
        yield ShowcaseScene(executable, Path(case.name))


@pytest.fixture(scope="class")
def keypad_executable(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Link the hardware consumer with controlled evdev and LED boundaries."""
    executable = tmp_path_factory.mktemp("showcase-keypad") / "showcase-keypad"
    drm_flags = shlex.split(
        run_process(
            ["pkg-config", "--cflags", "libdrm"],
            name="read DRM header flags for Showcase device doubles",
            timeout=10,
            check=True,
        ).stdout
    )
    run_process(
        [
            "cc",
            "-O2",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{APORT}",
            f"-I{SHARED}",
            str(ROOT / "tests/host_process/rootfs/fplinux-showcase-keypad.c"),
            str(APORT / "showcase-hardware.c"),
            str(ROOT / "lib/fplinux/fplinux-keypad-light.c"),
            str(ROOT / "lib/fplinux/fplinux-brightness-client.c"),
            str(FAKE_DEVICES),
            "-Wl,--wrap=open",
            "-Wl,--wrap=fopen",
            "-Wl,--wrap=ioctl",
            "-Wl,--wrap=read",
            "-Wl,--wrap=write",
            "-Wl,--wrap=connect",
            "-Wl,--wrap=close",
            *drm_flags,
            "-o",
            str(executable),
        ],
        name="compile Showcase keypad output consumer",
        timeout=30,
        check=True,
    )
    return executable


class FplinuxShowcaseBrightnessTests:
    """Run the scene with simulated evdev, DRM, and a local brightness service."""

    @pytest.mark.parametrize("renderer", [None, "auto"], ids=["default", "explicit-auto"])
    def test_auto_renderer_falls_back_after_gpu_init_error_and_restores_state(
        self, scene: ShowcaseScene, renderer: str | None
    ) -> None:
        """GPU setup failure leaves auto playback and borrowed hardware state usable."""
        requests = scene.run_showcase(renderer=renderer, gpu_init_failure=True)

        assert requests[0] == "CLAIM"
        assert requests[1].startswith("SHOW ")
        assert requests[2] == "RELEASE"
        assert len(requests) == 3

    def test_explicit_gpu_init_error_releases_brightness_and_restores_led(
        self, scene: ShowcaseScene
    ) -> None:
        """A required GPU startup error returns the acquired brightness and LED state."""
        requests = scene.run_showcase(renderer="gpu", gpu_init_failure=True)

        assert requests == ["CLAIM", "RELEASE"]

    @pytest.mark.parametrize(
        "led",
        [(1, 1, "ON 1\nOFF 0\n"), (255, 37, "ON 255\nOFF 0\n")],
        ids=["binary", "multilevel"],
    )
    def test_keypad_output_uses_full_brightness_and_restores_borrowed_state(
        self, keypad_executable: Path, tmp_path: Path, led: tuple[int, int, str]
    ) -> None:
        """Logical ON/OFF uses the LED's range and returns its borrowed state."""
        maximum, original, expected = led
        (tmp_path / "brightness").write_text(f"{original}\n", encoding="ascii")
        (tmp_path / "max_brightness").write_text(f"{maximum}\n", encoding="ascii")
        (tmp_path / "trigger").write_text("none [input-events] timer\n", encoding="ascii")
        environment = os.environ.copy()
        environment["SHOWCASE_KEYPAD_LED"] = str(tmp_path)

        result = run_process(
            [str(keypad_executable), str(tmp_path)],
            name="toggle Showcase keypad output and restore its borrowed state",
            timeout=5,
            env=environment,
        )

        assert result.returncode == 0, result.stderr
        assert result.stdout == expected
        assert (tmp_path / "brightness").read_text("ascii").splitlines()[0] == str(original)
        assert (tmp_path / "trigger").read_text("ascii") == "none [input-events] timer\n"

    @pytest.mark.parametrize("right_soft_key", ["event0", "event2"], ids=["first", "second"])
    def test_right_soft_key_exits_with_the_usable_keypad(
        self, scene: ShowcaseScene, right_soft_key: str
    ) -> None:
        """Only the usable member of two matching keypads can end the running scene."""
        requests = scene.run_showcase(right_soft_key=right_soft_key)

        assert (requests[0]) == ("CLAIM")
        assert requests[1].startswith("SHOW ")
        assert (requests[2]) == ("RELEASE")
        assert (len(requests)) == (3)

    @pytest.mark.parametrize("led_trigger", ["input-events", "none"], ids=["automatic", "manual"])
    def test_exit_restores_selected_trigger_with_led_initially_off(
        self, scene: ShowcaseScene, led_trigger: str
    ) -> None:
        """An initially dark LED retains automatic or manual ownership after the scene."""
        scene.run_showcase(led_trigger=led_trigger, led_brightness=0)

    def test_font_error_restores_manual_led_trigger(self, scene: ShowcaseScene) -> None:
        """A handled startup error preserves an LED with no automatic trigger."""
        scene.run_showcase(font_error="missing", led_trigger="none")

    @pytest.mark.parametrize("font_error", ["missing", "malformed"], ids=["missing", "malformed"])
    def test_missing_or_malformed_font_reports_error_and_releases_lease(
        self, scene: ShowcaseScene, font_error: str
    ) -> None:
        """Font startup errors restore the LED and the acquired brightness lease."""
        requests = scene.run_showcase(font_error=font_error)
        assert (requests) == (["CLAIM", "RELEASE"])

    def test_wrong_default_font_size_reports_error_and_releases_lease(
        self, scene: ShowcaseScene
    ) -> None:
        """An installed 8x16 default cannot silently render on the 128-pixel display."""
        assert (scene.run_showcase(font_error="wrong-size")) == (["CLAIM", "RELEASE"])

    def test_scene_previews_logical_level_and_releases_on_exit(self, scene: ShowcaseScene) -> None:
        """An active scene previews a logical level and gives back its lease."""
        requests = scene.run_showcase()

        assert (requests[0]) == ("CLAIM")
        assert (len(requests)) == (3)
        assert requests[1].startswith("SHOW ")
        assert (int(requests[1].split()[1])) in (range(6, 11))
        assert (requests[2]) == ("RELEASE")

    def test_unchanged_scene_level_is_not_resent_each_frame(self, scene: ShowcaseScene) -> None:
        """Two adjacent frames with the same level send only one preview."""
        requests = scene.run_showcase(extra_frame=True)

        assert (requests[0]) == ("CLAIM")
        assert (len([request for request in requests if request.startswith("SHOW ")])) == (1)
        assert (requests[-1]) == ("RELEASE")

    def test_display_activity_loss_releases_and_resume_reclaims_before_preview(
        self, scene: ShowcaseScene
    ) -> None:
        """Simulated display callbacks release and reclaim the brightness lease."""
        requests = scene.run_showcase(vt_cycle=True)

        assert (requests[0]) == ("CLAIM")
        assert requests[1].startswith("SHOW ")
        assert (requests[2:4]) == (["RELEASE", "CLAIM"])
        assert requests[4].startswith("SHOW ")
        assert (requests[5]) == ("RELEASE")
        assert (len(requests)) == (6)
