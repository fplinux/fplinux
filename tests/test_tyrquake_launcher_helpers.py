# SPDX-License-Identifier: GPL-2.0-only
"""Host test for the TyrQuake launcher's runtime cleanup helper."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tests/fplinux-tyrquake-launcher-helpers.c"
LAUNCHER = ROOT / "alpine/aports/fplinux-tyrquake/fplinux-quake.c"


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
            subprocess.run(
                [
                    "cc",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Dmain=fplinux_quake_program_main",
                    "-c",
                    str(LAUNCHER),
                    "-o",
                    str(launcher_object),
                ],
                check=True,
            )
            subprocess.run(
                [
                    "cc",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(HARNESS),
                    str(launcher_object),
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
