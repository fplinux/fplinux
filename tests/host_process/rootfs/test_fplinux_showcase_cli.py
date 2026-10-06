# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for FPLinux Showcase command-line parsing."""

from __future__ import annotations

import shlex
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests import ROOT
from tests.process import run_process

APORT = ROOT / "alpine/aports/fplinux-showcase"
SHARED = ROOT / "include/fplinux"
SOURCE = APORT / "fplinux-showcase.c"


class FplinuxShowcaseCliTests(unittest.TestCase):
    """Run the actual binary only through its argument and early-startup boundary."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Compile one strict host binary; no framebuffer or phone is supplied."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
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
                str(APORT / "armada-scene.c"),
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

    def test_help_returns_before_keypad_lookup(self) -> None:
        """Help lists the Showcase options and exits before any device lookup."""
        result = run_process(
            [str(self.executable), "--help"],
            name="show FPLinux Showcase help",
            timeout=5,
        )

        self.assertEqual(result.returncode, 0)
        self.assertIn("Usage:", result.stdout)
        self.assertIn("--runs", result.stdout)
        self.assertIn("--keypad-led", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_invalid_or_repeated_options_fail_before_keypad_lookup(self) -> None:
        """Bad run counts and repeated single-use options fail before any device lookup."""
        invalid_arguments = (
            ("--runs", "0"),
            ("--runs", "-1"),
            ("--runs", "+1"),
            ("--runs", "1ms"),
            ("--runs", "18446744073709551616"),
            ("--runs", "--help"),
            ("--runs", "1", "--runs", "2"),
            ("--keypad-led", "one", "--keypad-led", "two"),
        )

        for arguments in invalid_arguments:
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments],
                    name=f"reject FPLinux Showcase arguments {arguments}",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 2)
                self.assertIn("Try '", result.stderr)
                self.assertIn("--help' for more information.", result.stderr)
                self.assertNotIn("required keypad", result.stderr)
                self.assertEqual(result.stdout, "")

    def test_valid_options_reach_keypad_lookup(self) -> None:
        """Valid overrides are accepted; startup then fails to find the keypad on this host."""
        result = run_process(
            [
                str(self.executable),
                "--runs",
                "1",
                "--keypad-led",
                str(Path(self.temporary.name) / "keypad-led"),
            ],
            name="accept FPLinux Showcase options before keypad lookup",
            timeout=5,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required keypad fplinux/keypad0", result.stderr)
        self.assertNotIn("Try '", result.stderr)
        self.assertEqual(result.stdout, "")

    def compile_with_class_roots(self, class_root: Path) -> Path:
        """Compile the real application against a test-owned class tree."""
        executable = class_root / "fplinux-showcase"
        leds = class_root / "leds" / "*" / "brightness"
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
                str(APORT / "armada-scene.c"),
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
        class_root = Path(self.temporary.name) / "class-selected"
        self.add_class_device(class_root / "leds", "status")
        self.add_class_device(class_root / "leds", "panel:kbd_backlight")
        executable = self.compile_with_class_roots(class_root)

        result = run_process(
            [str(executable), "--runs", "1"],
            name="select FPLinux Showcase keyboard function LED",
            timeout=5,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required keypad fplinux/keypad0", result.stderr)
        self.assertNotIn("required keypad LED", result.stderr)

    def test_default_class_discovery_rejects_ambiguous_keyboard_leds(self) -> None:
        """Multiple kbd_backlight LED functions stop before any phone input opens."""
        class_root = Path(self.temporary.name) / "class-ambiguous"
        self.add_class_device(class_root / "leds", "left:kbd_backlight")
        self.add_class_device(class_root / "leds", "right:kbd_backlight")
        executable = self.compile_with_class_roots(class_root)

        result = run_process(
            [str(executable), "--runs", "1"],
            name="reject ambiguous FPLinux Showcase keyboard LEDs",
            timeout=5,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ambiguous keypad LEDs", result.stderr)
        self.assertNotIn("required keypad fplinux/keypad0", result.stderr)


if __name__ == "__main__":
    unittest.main()
