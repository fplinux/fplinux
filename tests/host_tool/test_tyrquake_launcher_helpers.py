# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for TyrQuake launcher arguments and runtime cleanup."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tests/host_tool/fplinux-tyrquake-launcher-helpers.c"
LAUNCHER = ROOT / "alpine/aports/fplinux-tyrquake/fplinux-quake.c"
SHARED = ROOT / "lib/fplinux/fplinux-fb-session.c"
SHARED_INCLUDE = ROOT / "include/fplinux"


class TyrQuakeLauncherHelperTests(unittest.TestCase):
    """Exercise cleanup code without claiming launcher or device coverage."""

    def test_remove_runtime_deletes_tree_without_following_pak_symlink(self) -> None:
        """Cleanup removes volatile files without deleting external game data."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = root / "fplinux-quake.123"
            game = runtime / "id1"
            user_game = runtime / ".tyrquake/id1"
            game.mkdir(parents=True)
            user_game.mkdir(parents=True)
            pak = root / "pak0.pak"
            pak.write_bytes(b"pak\n")
            (game / "pak0.pak").symlink_to(pak)
            (game / "config.cfg").write_text("phone controls\n", encoding="utf-8")
            (user_game / "config.cfg").write_text("engine config\n", encoding="utf-8")
            (user_game / "video.cfg").write_text("video config\n", encoding="utf-8")
            (user_game / "save-game.dat").write_text("save game\n", encoding="utf-8")

            executable = root / "cleanup-harness"
            launcher_object = root / "fplinux-quake.o"
            run_process(
                [
                    "cc",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{SHARED_INCLUDE}",
                    "-Dmain=fplinux_quake_program_main",
                    "-c",
                    str(LAUNCHER),
                    "-o",
                    str(launcher_object),
                ],
                name="compile TyrQuake launcher object",
                timeout=30,
                check=True,
            )
            run_process(
                [
                    "cc",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(HARNESS),
                    f"-I{SHARED_INCLUDE}",
                    str(launcher_object),
                    str(SHARED),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    "-o",
                    str(executable),
                ],
                name="link TyrQuake cleanup harness",
                timeout=30,
                check=True,
            )

            run_process(
                [str(executable), str(runtime)],
                name="run TyrQuake cleanup harness",
                timeout=10,
                check=True,
            )

            self.assertFalse(runtime.exists())
            self.assertEqual(pak.read_bytes(), b"pak\n")


class TyrQuakeLauncherCliTests(unittest.TestCase):
    """Run only help and invalid CLI paths of the host-compiled launcher."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Link the production entry point without replacing its parser."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "quake"
        run_process(
            [
                "cc",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{SHARED_INCLUDE}",
                str(LAUNCHER),
                str(SHARED),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                "-o",
                str(cls.executable),
            ],
            name="compile TyrQuake launcher CLI",
            timeout=30,
            check=True,
        )

    def test_help_exits_without_game_data_or_display(self) -> None:
        """Parsed help takes priority over missing, invalid or extra options."""
        for arguments in (
            ("-h",),
            ("--help",),
            ("--input", "phone", "--help"),
            ("--input=keyboard", "-h"),
            ("--input=invalid", "--help"),
            ("--unknown", "--help"),
        ):
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments],
                    name="run TyrQuake launcher help",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Usage:", result.stdout)
                self.assertIn("--input", result.stdout)
                self.assertEqual(result.stderr, "")

    def test_invalid_syntax_has_no_game_output(self) -> None:
        """Missing, repeated or extra arguments return the CLI error status."""
        for arguments in (
            (),
            ("--input",),
            ("--unknown",),
            ("--input", "phone", "extra"),
            ("--input=phone", "--input=keyboard"),
            ("--", "--help"),
        ):
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments],
                    name="run TyrQuake launcher syntax error",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("--help", result.stderr)

    def test_input_spellings_reach_control_validation(self) -> None:
        """Separate, equal and abbreviated option forms share value validation."""
        for arguments in (
            ("--input", "invalid"),
            ("--input=invalid",),
            ("--in=invalid",),
            ("--input", "--help"),
        ):
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments],
                    name="run TyrQuake launcher input error",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("--input must be phone or keyboard", result.stderr)

    def test_heap_size_outside_the_supported_range_is_refused(self) -> None:
        """A size the engine cannot use is rejected before any device is touched."""
        for value in ("0", "8191", "262145", "32768x", "-1", "", "1e5"):
            with self.subTest(value=value):
                result = run_process(
                    [str(self.executable), "--input", "phone", f"--heapsize={value}"],
                    name="run TyrQuake launcher heap error",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("--heapsize", result.stderr)

    def test_help_lists_the_heap_size_and_its_default(self) -> None:
        """The size is part of the published interface, not a hidden constant."""
        result = run_process(
            [str(self.executable), "--help"],
            name="run TyrQuake launcher help",
            timeout=5,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--heapsize", result.stdout)
        self.assertIn("32768", result.stdout)


if __name__ == "__main__":
    unittest.main()
