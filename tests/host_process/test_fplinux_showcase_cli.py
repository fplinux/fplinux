# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for FPLinux Showcase command-line parsing."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
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
                str(ROOT / "lib/fplinux/fplinux-fb-session.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                "-o",
                str(cls.executable),
            ],
            name="compile FPLinux Showcase command-line boundary",
            timeout=30,
            check=True,
        )

    def test_help_returns_before_opening_phone_hardware(self) -> None:
        """A parsed help option wins over later parser and domain errors."""
        help_arguments = (
            ("-h",),
            ("--help",),
            ("--help", "--runs=0"),
            ("--help", "--unknown"),
        )

        for arguments in help_arguments:
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments],
                    name=f"show FPLinux Showcase help with {arguments}",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 0)
                self.assertIn("Usage:", result.stdout)
                self.assertIn("--runs", result.stdout)
                self.assertIn("--keypad-led", result.stdout)
                self.assertIn("--lcd-backlight", result.stdout)
                self.assertEqual(result.stderr, "")

    def test_invalid_arguments_fail_before_opening_phone_hardware(self) -> None:
        """Malformed options return native diagnostics without probing keypad or framebuffer."""
        invalid_arguments = (
            ("--runs",),
            ("--runs", "0"),
            ("--runs", "-1"),
            ("--runs", "+1"),
            ("--runs", "1ms"),
            ("--runs", "18446744073709551616"),
            ("--runs", "1", "extra"),
            ("--runs", "--help"),
            ("--runs", "1", "--runs", "2"),
            ("--keypad-led", "one", "--keypad-led", "two"),
            ("--lcd-backlight", "one", "--lcd-backlight", "two"),
            ("--keypad-led=",),
            ("--lcd-backlight=",),
            ("--unknown",),
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

    def test_valid_options_reach_the_real_hardware_boundary(self) -> None:
        """Valid overrides are accepted before the host lacks the phone keypad."""
        result = run_process(
            [
                str(self.executable),
                "--runs",
                "1",
                "--keypad-led",
                str(Path(self.temporary.name) / "keypad-led"),
                "--lcd-backlight",
                str(Path(self.temporary.name) / "lcd-backlight"),
            ],
            name="accept FPLinux Showcase options before phone hardware startup",
            timeout=5,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required keypad fplinux/keypad0", result.stderr)
        self.assertNotIn("usage: fplinux-showcase", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_native_long_options_reach_the_real_hardware_boundary(self) -> None:
        """Equals-delimited options and an unambiguous native prefix are accepted."""
        keypad_led = Path(self.temporary.name) / "keypad-led"
        lcd_backlight = Path(self.temporary.name) / "lcd-backlight"

        for runs_option in ("--runs=1", "--r=1"):
            with self.subTest(runs_option=runs_option):
                result = run_process(
                    [
                        str(self.executable),
                        runs_option,
                        f"--keypad-led={keypad_led}",
                        f"--lcd-backlight={lcd_backlight}",
                    ],
                    name="accept native FPLinux Showcase long options",
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
        backlights = class_root / "backlights" / "*" / "brightness"
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
                f'-DFPLINUX_SHOWCASE_LCD_BACKLIGHT_GLOB="{backlights}"',
                str(SOURCE),
                str(APORT / "armada-scene.c"),
                str(ROOT / "lib/fplinux/fplinux-fb-session.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
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

    def test_default_class_discovery_uses_keyboard_led_function(self) -> None:
        """A status LED does not replace the sole kbd_backlight function LED."""
        class_root = Path(self.temporary.name) / "class-selected"
        self.add_class_device(class_root / "leds", "status")
        self.add_class_device(class_root / "leds", "panel:kbd_backlight")
        self.add_class_device(class_root / "backlights", "lcd")
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

    def test_default_class_discovery_rejects_ambiguous_lcd_backlights(self) -> None:
        """Multiple display backlights stop before any phone input opens."""
        class_root = Path(self.temporary.name) / "backlight-ambiguous"
        self.add_class_device(class_root / "leds", "kbd_backlight")
        self.add_class_device(class_root / "backlights", "lcd0")
        self.add_class_device(class_root / "backlights", "lcd1")
        executable = self.compile_with_class_roots(class_root)

        result = run_process(
            [str(executable), "--runs", "1"],
            name="reject ambiguous FPLinux Showcase LCD backlights",
            timeout=5,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ambiguous LCD backlights", result.stderr)
        self.assertNotIn("required keypad fplinux/keypad0", result.stderr)


if __name__ == "__main__":
    unittest.main()
