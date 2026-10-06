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
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

import pytest

if TYPE_CHECKING:
    from collections.abc import Iterator

from tests import ROOT
from tests.fixtures.psf_font import write_solid_ascii_font
from tests.process import run_process

APORT = ROOT / "alpine/aports/fplinux-showcase"
SHARED = ROOT / "include/fplinux"
FAKE_DEVICES = ROOT / "tests/host_process/rootfs/fplinux-showcase-fake-devices.c"


class FplinuxShowcaseBrightnessTests:
    """Run the scene with simulated evdev, DRM, and a local brightness service."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_binaries(cls) -> Iterator[None]:
        """Compile the real Showcase command with simulated device boundaries."""
        with ExitStack() as cleanup:
            cls.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(cls.temporary)
            cls.executable = Path(cls.temporary.name) / "fplinux-showcase"
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
                    f"-I{APORT}",
                    f"-I{SHARED}",
                    str(APORT / "fplinux-showcase.c"),
                    str(APORT / "showcase-hardware.c"),
                    str(APORT / "armada-scene.c"),
                    str(APORT / "armada-storyboard.c"),
                    str(APORT / "armada-renderer.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
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
                    *drm_flags,
                    "-o",
                    str(cls.executable),
                ],
                name="compile Showcase against controlled device boundaries",
                timeout=30,
                check=True,
            )
            yield

    @pytest.fixture(autouse=True)
    def _prepare_scene(self) -> Iterator[None]:
        """Give each scene its own LED, font and Unix socket paths."""
        with ExitStack() as cleanup:
            case = tempfile.TemporaryDirectory()
            cleanup.enter_context(case)
            self.work = Path(case.name)
            yield

    def run_showcase(
        self,
        *,
        extra_frame: bool = False,
        vt_cycle: bool = False,
        font_error: str | None = None,
        led_trigger: str = "input-events",
        led_brightness: int = 1,
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

        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as server:
            server.bind(str(socket_path))
            server.listen(1)
            server.settimeout(5)

            def serve() -> None:
                try:
                    connection, _ = server.accept()
                    with connection:
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
                if extra_frame or vt_cycle:
                    environment["SHOWCASE_EXTRA_FRAME"] = "1"
                if vt_cycle:
                    environment["SHOWCASE_VT_CYCLE"] = "1"
                result = run_process(
                    [str(self.executable), "--keypad-led", str(keypad_led)],
                    name="render Showcase with a local brightness service",
                    timeout=10,
                    env=environment,
                )
            finally:
                worker.join(timeout=5)

        assert not (worker.is_alive()), "brightness service did not stop"
        assert (server_errors) == ([])
        if font_error:
            assert (result.returncode) == (1), result.stderr
            if font_error == "wrong-size":
                assert ("default font is 8x16; 128-pixel display requires 6x12") in (result.stderr)
            else:
                assert ("cannot load font /usr/share/fplinux/fonts/default.psf") in (result.stderr)
            assert (result.stdout) == ("")
        else:
            assert (result.returncode) == (0), result.stderr
        assert ((keypad_led / "brightness").read_text(encoding="ascii").splitlines()[0]) == (
            str(led_brightness)
        )
        selected = re.findall(r"\[([^\]]+)\]", (keypad_led / "trigger").read_text("ascii"))
        assert (selected) == ([led_trigger])
        return requests

    @pytest.mark.parametrize("led_trigger", ["input-events", "none"], ids=["automatic", "manual"])
    def test_exit_restores_selected_trigger_with_led_initially_off(self, led_trigger: str) -> None:
        """An initially dark LED retains automatic or manual ownership after the scene."""
        self.run_showcase(led_trigger=led_trigger, led_brightness=0)

    def test_font_error_restores_manual_led_trigger(self) -> None:
        """A handled startup error preserves an LED with no automatic trigger."""
        self.run_showcase(font_error="missing", led_trigger="none")

    @pytest.mark.parametrize("font_error", ["missing", "malformed"], ids=["missing", "malformed"])
    def test_missing_or_malformed_font_reports_error_and_releases_lease(
        self, font_error: str
    ) -> None:
        """Font startup errors restore the LED and the acquired brightness lease."""
        requests = self.run_showcase(font_error=font_error)
        assert (requests) == (["CLAIM", "RELEASE"])

    def test_wrong_default_font_size_reports_error_and_releases_lease(self) -> None:
        """An installed 8x16 default cannot silently render on the 128-pixel display."""
        assert (self.run_showcase(font_error="wrong-size")) == (["CLAIM", "RELEASE"])

    def test_scene_previews_logical_level_and_releases_on_exit(self) -> None:
        """An active scene previews a logical level and gives back its lease."""
        requests = self.run_showcase()

        assert (requests[0]) == ("CLAIM")
        assert (len(requests)) == (3)
        assert requests[1].startswith("SHOW ")
        assert (int(requests[1].split()[1])) in (range(6, 11))
        assert (requests[2]) == ("RELEASE")

    def test_unchanged_scene_level_is_not_resent_each_frame(self) -> None:
        """Two adjacent frames with the same level send only one preview."""
        requests = self.run_showcase(extra_frame=True)

        assert (requests[0]) == ("CLAIM")
        assert (len([request for request in requests if request.startswith("SHOW ")])) == (1)
        assert (requests[-1]) == ("RELEASE")

    def test_vt_loss_releases_and_resume_claims_before_preview(self) -> None:
        """Losing the display releases the lease; resumed rendering reclaims it."""
        requests = self.run_showcase(vt_cycle=True)

        assert (requests[0]) == ("CLAIM")
        assert requests[1].startswith("SHOW ")
        assert (requests[2:4]) == (["RELEASE", "CLAIM"])
        assert requests[4].startswith("SHOW ")
        assert (requests[5]) == ("RELEASE")
        assert (len(requests)) == (6)
