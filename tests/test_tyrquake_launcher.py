# SPDX-License-Identifier: GPL-2.0-only
"""Focused regressions for the FPLinux TyrQuake launcher."""

from __future__ import annotations

import json
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "alpine/aports/fplinux-tyrquake/fplinux-quake.c"


class TyrQuakeLauncherTests(unittest.TestCase):
    """Exercise launcher behavior that does not require framebuffer hardware."""

    def test_cleanup_removes_the_complete_volatile_runtime(self) -> None:
        """A clean exit must not retain engine configs, saves, or PAK links."""
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

            harness = root / "cleanup-harness.c"
            harness.write_text(
                textwrap.dedent(
                    f"""
                    #define main fplinux_quake_program_main
                    #include {json.dumps(str(LAUNCHER))}
                    #undef main

                    int main(int argc, char **argv)
                    {{
                            if (argc != 2)
                                    return 2;
                            remove_runtime(argv[1]);
                            return access(argv[1], F_OK) == -1 && errno == ENOENT
                                           ? 0
                                           : 1;
                    }}
                    """
                ),
                encoding="utf-8",
            )
            executable = root / "cleanup-harness"
            subprocess.run(
                [
                    "cc",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(harness),
                    "-o",
                    str(executable),
                ],
                check=True,
            )

            subprocess.run([str(executable), str(runtime)], check=True)

            self.assertFalse(runtime.exists())
            self.assertEqual(pak.read_bytes(), b"pak\n")


if __name__ == "__main__":
    unittest.main()
