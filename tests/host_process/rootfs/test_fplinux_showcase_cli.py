# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for FPLinux Showcase command-line parsing."""

from __future__ import annotations

import shlex
import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

import pytest

if TYPE_CHECKING:
    from collections.abc import Iterator

from tests import ROOT
from tests.process import run_process

APORT = ROOT / "alpine/aports/fplinux-showcase"
SHARED = ROOT / "include/fplinux"
SOURCE = APORT / "fplinux-showcase.c"


class FplinuxShowcaseCliTests:
    """Run the actual binary only through its argument and early-startup boundary."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]
    discovery_root: ClassVar[Path]
    discovery_executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_binaries(cls) -> Iterator[None]:
        """Compile strict host binaries; no framebuffer or phone is supplied."""
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
                    str(SOURCE),
                    str(APORT / "showcase-hardware.c"),
                    str(APORT / "armada-scene.c"),
                    str(APORT / "armada-storyboard.c"),
                    str(APORT / "armada-renderer.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                    str(ROOT / "lib/fplinux/fplinux-drm-session.c"),
                    str(ROOT / "lib/fplinux/fplinux-brightness-client.c"),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    *drm_flags,
                    "-o",
                    str(cls.executable),
                ],
                name="compile FPLinux Showcase command-line boundary",
                timeout=30,
                check=True,
            )
            cls.discovery_root = Path(cls.temporary.name) / "class-discovery"
            (cls.discovery_root / "cases").mkdir(parents=True)
            cls.discovery_executable = cls.compile_with_class_roots(cls.discovery_root)
            yield

    @pytest.fixture(autouse=True)
    def _prepare_class_tree(self, _compiled_binaries: None) -> Iterator[None]:
        """Give each invocation a fresh class tree consumed by the compiled glob."""
        with ExitStack() as cleanup:
            case = tempfile.TemporaryDirectory(dir=self.discovery_root / "cases")
            cleanup.enter_context(case)
            self.work = Path(case.name)
            yield

    def test_help_returns_before_keypad_lookup(self) -> None:
        """Help lists the Showcase options and exits before any device lookup."""
        result = run_process(
            [str(self.executable), "--help"],
            name="show FPLinux Showcase help",
            timeout=5,
        )

        assert (result.returncode) == (0)
        assert ("Usage:") in (result.stdout)
        assert ("--runs") in (result.stdout)
        assert ("--keypad-led") in (result.stdout)
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
        ],
    )
    def test_invalid_or_repeated_options_fail_before_keypad_lookup(
        self, arguments: tuple[str, ...]
    ) -> None:
        """Bad run counts and repeated single-use options fail before any device lookup."""
        result = run_process(
            [str(self.executable), *arguments],
            name=f"reject FPLinux Showcase arguments {arguments}",
            timeout=5,
        )

        assert (result.returncode) == (2)
        assert ("Try '") in (result.stderr)
        assert ("--help' for more information.") in (result.stderr)
        assert ("required keypad") not in (result.stderr)
        assert (result.stdout) == ("")

    def test_valid_options_reach_keypad_lookup(self) -> None:
        """Valid overrides are accepted; startup then fails to find the keypad on this host."""
        result = run_process(
            [
                str(self.executable),
                "--runs",
                "1",
                "--keypad-led",
                str(self.work / "keypad-led"),
            ],
            name="accept FPLinux Showcase options before keypad lookup",
            timeout=5,
        )

        assert (result.returncode) != (0)
        assert ("required keypad fplinux/keypad0") in (result.stderr)
        assert ("Try '") not in (result.stderr)
        assert (result.stdout) == ("")

    @staticmethod
    def compile_with_class_roots(class_root: Path) -> Path:
        """Compile the real application against a test-owned class tree."""
        executable = class_root / "fplinux-showcase"
        leds = class_root / "cases" / "*" / "leds" / "*" / "brightness"
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
                f'-DFPLINUX_SHOWCASE_KEYPAD_LED_GLOB="{leds}"',
                str(SOURCE),
                str(APORT / "showcase-hardware.c"),
                str(APORT / "armada-scene.c"),
                str(APORT / "armada-storyboard.c"),
                str(APORT / "armada-renderer.c"),
                str(ROOT / "lib/fplinux/fplinux-font.c"),
                str(ROOT / "lib/fplinux/fplinux-drm-session.c"),
                str(ROOT / "lib/fplinux/fplinux-brightness-client.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                *drm_flags,
                "-o",
                str(executable),
            ],
            name="compile FPLinux Showcase class discovery",
            timeout=30,
            check=True,
        )
        return executable

    def add_class_device(self, root: Path, name: str) -> None:
        """Create one controlled brightness class device."""
        device = root / name
        device.mkdir(parents=True)
        (device / "brightness").write_text("1\n", encoding="ascii")
        (device / "max_brightness").write_text("10\n", encoding="ascii")
        (device / "trigger").write_text("[none] input-events\n", encoding="ascii")

    def test_default_class_discovery_uses_keyboard_led_function(self) -> None:
        """A status LED does not replace the sole kbd_backlight function LED."""
        class_root = self.work
        self.add_class_device(class_root / "leds", "status")
        self.add_class_device(class_root / "leds", "panel:kbd_backlight")
        executable = self.discovery_executable

        result = run_process(
            [str(executable), "--runs", "1"],
            name="select FPLinux Showcase keyboard function LED",
            timeout=5,
        )

        assert (result.returncode) != (0)
        assert ("required keypad fplinux/keypad0") in (result.stderr)
        assert ("required keypad LED") not in (result.stderr)

    def test_default_class_discovery_rejects_ambiguous_keyboard_leds(self) -> None:
        """Multiple kbd_backlight LED functions stop before any phone input opens."""
        class_root = self.work
        self.add_class_device(class_root / "leds", "left:kbd_backlight")
        self.add_class_device(class_root / "leds", "right:kbd_backlight")
        executable = self.discovery_executable

        result = run_process(
            [str(executable), "--runs", "1"],
            name="reject ambiguous FPLinux Showcase keyboard LEDs",
            timeout=5,
        )

        assert (result.returncode) != (0)
        assert ("ambiguous keypad LEDs") in (result.stderr)
        assert ("required keypad fplinux/keypad0") not in (result.stderr)
