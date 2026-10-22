# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for FPLinux Showcase command-line parsing."""

from __future__ import annotations

import os
import shlex
import tempfile
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

if TYPE_CHECKING:
    from collections.abc import Iterator

from tests import ROOT
from tests.process import run_process

APORT = ROOT / "alpine/aports/fplinux-showcase"
SHARED = ROOT / "include/fplinux"
SOURCE = APORT / "fplinux-showcase.c"
FAKE_DEVICES = ROOT / "tests/host_process/rootfs/fplinux-showcase-fake-devices.c"


@dataclass(frozen=True)
class ShowcaseBinaries:
    """Immutable executables and the root compiled into class discovery."""

    executable: Path
    discovery_root: Path
    discovery_executable: Path


def compile_with_class_roots(class_root: Path) -> Path:
    """Compile real class discovery with simulated input and display devices."""
    executable = class_root / "fplinux-showcase"
    leds = class_root / "cases" / "*" / "leds"
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
            f'-DFPLINUX_KEYPAD_LED_CLASS_DIR="{leds}"',
            str(SOURCE),
            str(APORT / "showcase-hardware.c"),
            str(APORT / "armada-scene.c"),
            str(APORT / "armada-storyboard.c"),
            str(APORT / "armada-renderer.c"),
            str(APORT / "armada-geometry.c"),
            str(APORT / "armada-overlay.c"),
            str(APORT / "armada-gpu-renderer.c"),
            str(ROOT / "lib/fplinux/fplinux-font.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
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
        name="compile FPLinux Showcase class discovery",
        timeout=30,
        check=True,
    )
    return executable


def add_class_device(root: Path, name: str) -> None:
    """Create one controlled brightness class device."""
    device = root / name
    device.mkdir(parents=True)
    (device / "brightness").write_text("1\n", encoding="ascii")
    (device / "max_brightness").write_text("10\n", encoding="ascii")
    (device / "trigger").write_text("[none] input-events\n", encoding="ascii")


@pytest.fixture(scope="class")
def binaries() -> Iterator[ShowcaseBinaries]:
    """Compile strict host binaries; no framebuffer or phone is supplied."""
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
                str(SOURCE),
                str(APORT / "showcase-hardware.c"),
                str(APORT / "armada-scene.c"),
                str(APORT / "armada-storyboard.c"),
                str(APORT / "armada-renderer.c"),
                str(APORT / "armada-geometry.c"),
                str(APORT / "armada-overlay.c"),
                str(APORT / "armada-gpu-renderer.c"),
                str(ROOT / "lib/fplinux/fplinux-font.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                str(ROOT / "lib/fplinux/fplinux-gpu.c"),
                str(ROOT / "lib/fplinux/fplinux-gpu-output.c"),
                str(ROOT / "lib/fplinux/fplinux-keypad-light.c"),
                str(ROOT / "lib/fplinux/fplinux-drm-session.c"),
                str(ROOT / "lib/fplinux/fplinux-brightness-client.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                "-ldl",
                *drm_flags,
                "-o",
                str(executable),
            ],
            name="compile FPLinux Showcase command-line boundary",
            timeout=30,
            check=True,
        )
        discovery_root = Path(temporary.name) / "class-discovery"
        (discovery_root / "cases").mkdir(parents=True)
        discovery_executable = compile_with_class_roots(discovery_root)
        yield ShowcaseBinaries(executable, discovery_root, discovery_executable)


@pytest.fixture
def class_tree(binaries: ShowcaseBinaries) -> Iterator[Path]:
    """Give each invocation a fresh class tree consumed by the compiled glob."""
    with ExitStack() as cleanup:
        case = tempfile.TemporaryDirectory(dir=binaries.discovery_root / "cases")
        cleanup.enter_context(case)
        yield Path(case.name)


class FplinuxShowcaseCliTests:
    """Run the actual binary only through its argument and early-startup boundary."""

    @pytest.mark.parametrize(
        "arguments",
        [("--help",), ("--renderer", "invalid", "--help")],
        ids=["plain", "invalid-renderer"],
    )
    def test_help_returns_before_keypad_lookup(
        self, binaries: ShowcaseBinaries, arguments: tuple[str, ...]
    ) -> None:
        """Help lists the Showcase options and exits before any device lookup."""
        result = run_process(
            [str(binaries.executable), *arguments],
            name="show FPLinux Showcase help",
            timeout=5,
        )

        assert (result.returncode) == (0)
        assert ("Usage:") in (result.stdout)
        assert ("--runs") in (result.stdout)
        assert ("--keypad-led") in (result.stdout)
        assert "--renderer" in result.stdout
        assert "auto" in result.stdout
        assert "software" in result.stdout
        assert "gpu" in result.stdout
        assert (result.stderr) == ("")

    @pytest.mark.parametrize(
        "arguments",
        [
            ("--runs", "0"),
            ("--runs", "-1"),
            ("--runs", "+1"),
            ("--runs", "1ms"),
            ("--runs", "18446744073709551616"),
            ("--runs", "--help"),
            ("--runs", "1", "--runs", "2"),
            ("--keypad-led", "one", "--keypad-led", "two"),
            ("--renderer", "vulkan"),
            ("--renderer", ""),
            ("--renderer", "GPU"),
            ("--renderer",),
            ("--renderer", "auto", "--renderer", "software"),
        ],
        ids=[
            "zero-runs",
            "negative-runs",
            "plus-runs",
            "run-suffix",
            "run-overflow",
            "missing-run-value",
            "repeated-runs",
            "repeated-keypad-led",
            "unknown-renderer",
            "empty-renderer",
            "uppercase-renderer",
            "missing-renderer-value",
            "repeated-renderer",
        ],
    )
    def test_invalid_or_repeated_options_fail_before_keypad_lookup(
        self, binaries: ShowcaseBinaries, arguments: tuple[str, ...]
    ) -> None:
        """Invalid values and repeated single-use options fail before any device lookup."""
        result = run_process(
            [str(binaries.executable), *arguments],
            name=f"reject FPLinux Showcase arguments {arguments}",
            timeout=5,
        )

        assert (result.returncode) == (2)
        assert ("Try '") in (result.stderr)
        assert ("--help' for more information.") in (result.stderr)
        assert ("required keypad") not in (result.stderr)
        assert (result.stdout) == ("")

    @pytest.mark.parametrize("renderer", ["auto", "software", "gpu"])
    def test_valid_renderer_options_reach_keypad_lookup(
        self, binaries: ShowcaseBinaries, class_tree: Path, renderer: str
    ) -> None:
        """Valid overrides are accepted; startup then fails to find the keypad on this host."""
        result = run_process(
            [
                str(binaries.executable),
                "--runs",
                "1",
                "--renderer",
                renderer,
                "--keypad-led",
                str(class_tree / "keypad-led"),
            ],
            name="accept FPLinux Showcase options before keypad lookup",
            timeout=5,
        )

        assert (result.returncode) != (0)
        assert ("required keypad fplinux/keypad0") in (result.stderr)
        assert ("Try '") not in (result.stderr)
        assert (result.stdout) == ("")

    def test_default_class_discovery_rejects_unusable_keyboard_led(
        self, binaries: ShowcaseBinaries, class_tree: Path
    ) -> None:
        """Startup validates the keyboard LED even when another class LED is usable."""
        class_root = class_tree
        add_class_device(class_root / "leds", "aaa-status")
        add_class_device(class_root / "leds", "panel:kbd_backlight")
        keyboard_led = class_root / "leds/panel:kbd_backlight"
        (keyboard_led / "max_brightness").write_text("0\n", encoding="ascii")

        environment = os.environ.copy()
        environment["SHOWCASE_KEYPAD_LED"] = str(keyboard_led)
        environment["SHOWCASE_BRIGHTNESS_SOCKET"] = str(class_root / "absent-brightness.sock")
        executable = binaries.discovery_executable

        result = run_process(
            [str(executable), "--runs", "1"],
            name="reject FPLinux Showcase keyboard LED with zero maximum",
            timeout=5,
            env=environment,
        )

        assert (result.returncode) == (1)
        assert ("required keypad LED interface is unavailable") in (result.stderr)
        assert ("display activation") not in (result.stderr)

    def test_default_class_discovery_rejects_ambiguous_keyboard_leds(
        self, binaries: ShowcaseBinaries, class_tree: Path
    ) -> None:
        """Multiple kbd_backlight LED functions stop before any phone input opens."""
        class_root = class_tree
        add_class_device(class_root / "leds", "left:kbd_backlight")
        add_class_device(class_root / "leds", "right:kbd_backlight")
        executable = binaries.discovery_executable

        result = run_process(
            [str(executable), "--runs", "1"],
            name="reject ambiguous FPLinux Showcase keyboard LEDs",
            timeout=5,
        )

        assert (result.returncode) != (0)
        assert ("ambiguous keypad LEDs") in (result.stderr)
        assert ("required keypad fplinux/keypad0") not in (result.stderr)

    def test_keypad_discovery_rejects_devices_without_the_right_soft_key(
        self, binaries: ShowcaseBinaries, class_tree: Path
    ) -> None:
        """Matching physical paths do not make a keypad without the exit key usable."""
        add_class_device(class_tree / "leds", "panel:kbd_backlight")
        keyboard_led = class_tree / "leds/panel:kbd_backlight"
        environment = os.environ.copy()
        environment["SHOWCASE_KEYPAD_LED"] = str(keyboard_led)
        environment["SHOWCASE_RIGHT_SOFT_KEY"] = "none"

        result = run_process(
            [str(binaries.discovery_executable), "--runs", "1"],
            name="reject Showcase keypads without an exit key",
            timeout=5,
            env=environment,
        )

        assert (result.returncode) == (1)
        assert ("keypad does not expose the right soft key") in (result.stderr)
        assert ("required keypad LED interface is unavailable") not in (result.stderr)
        assert (result.stdout) == ("")
        assert ((keyboard_led / "brightness").read_text(encoding="ascii")) == ("1\n")
        assert ((keyboard_led / "trigger").read_text(encoding="ascii")) == (
            "[none] input-events\n"
        )
