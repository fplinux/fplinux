# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for TyrQuake launcher arguments and runtime cleanup."""

from __future__ import annotations

import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

HARNESS = ROOT / "tests/host_tool/quake/fplinux-tyrquake-launcher-helpers.c"
LAUNCHER = ROOT / "alpine/aports/fplinux-tyrquake/fplinux-quake.c"
SHARED_INCLUDE = ROOT / "include/fplinux"


class TyrQuakeLauncherHelperTests:
    """Exercise cleanup code without claiming launcher or device coverage."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the cleanup harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            cls.executable = Path(build_directory.name) / "cleanup-harness"
            launcher_object = Path(build_directory.name) / "fplinux-quake.o"
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
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    "-o",
                    str(cls.executable),
                ],
                name="link TyrQuake cleanup harness",
                timeout=30,
                check=True,
            )
            yield

    def test_remove_runtime_deletes_tree_and_keeps_linked_game_data(self) -> None:
        """Cleanup removes the runtime tree and its file links, not their targets."""
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
            (game / "config.cfg").write_text("game controls\n", encoding="utf-8")
            (user_game / "config.cfg").write_text("engine config\n", encoding="utf-8")
            (user_game / "video.cfg").write_text("video config\n", encoding="utf-8")
            (user_game / "save-game.dat").write_text("save game\n", encoding="utf-8")

            run_process(
                [str(self.executable), str(runtime)],
                name="run TyrQuake cleanup harness",
                timeout=10,
                check=True,
            )

            assert not (runtime.exists())
            assert (pak.read_bytes()) == (b"pak\n")


class TyrQuakeLauncherCliTests:
    """Run only help and invalid CLI paths of the host-compiled launcher."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the production entry point without replacing its parser."""
        with ExitStack() as cleanup:
            cls.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(cls.temporary)
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
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    "-o",
                    str(cls.executable),
                ],
                name="compile TyrQuake launcher CLI",
                timeout=30,
                check=True,
            )
            yield

    @pytest.mark.parametrize(
        "arguments",
        [
            ("-h",),
            ("--help",),
            ("--heapsize=invalid", "--help"),
            ("--unknown", "--help"),
            ("extra", "-h"),
        ],
        ids=["h", "help", "heapsize-invalid-help", "unknown-help", "extra-h"],
    )
    def test_help_exits_without_game_data_or_display(self, arguments: tuple[str, ...]) -> None:
        """Parsed help takes priority over invalid or extra arguments."""
        result = run_process(
            [str(self.executable), *arguments],
            name="run TyrQuake launcher help",
            timeout=5,
        )

        assert (result.returncode) == (0), result.stderr
        assert ("Usage:") in (result.stdout)
        assert (result.stderr) == ("")

    @pytest.mark.parametrize(
        "arguments",
        [
            ("--unknown",),
            ("--heapsize",),
            ("extra",),
            ("--heapsize=16384", "extra"),
            ("--", "--help"),
        ],
        ids=["unknown", "heapsize", "extra", "heapsize-16384-extra", "separator-help"],
    )
    def test_invalid_syntax_has_no_game_output(self, arguments: tuple[str, ...]) -> None:
        """Unknown options, missing values and extra arguments are refused."""
        result = run_process(
            [str(self.executable), *arguments],
            name="run TyrQuake launcher syntax error",
            timeout=5,
        )

        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("--help") in (result.stderr)

    @pytest.mark.parametrize(
        "value",
        ["0", "8191", "262145", "32768x", "-1", "", "1e5"],
        ids=["0", "8191", "262145", "32768x", "negative-one", "empty", "1e5"],
    )
    def test_heap_size_outside_the_supported_range_is_refused(self, value: str) -> None:
        """A size the engine cannot use is rejected before any device is touched."""
        result = run_process(
            [str(self.executable), f"--heapsize={value}"],
            name="run TyrQuake launcher heap error",
            timeout=5,
        )

        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("--heapsize") in (result.stderr)

    def test_help_lists_the_heap_size_and_its_default(self) -> None:
        """The size is part of the published interface, not a hidden constant."""
        result = run_process(
            [str(self.executable), "--help"],
            name="run TyrQuake launcher help",
            timeout=5,
        )

        assert (result.returncode) == (0), result.stderr
        assert ("--heapsize") in (result.stdout)
        assert ("32768") in (result.stdout)
