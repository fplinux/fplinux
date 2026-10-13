# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for TyrQuake launcher arguments and runtime cleanup."""

from __future__ import annotations

import stat
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

HARNESS = ROOT / "tests/host_tool/quake/fplinux-tyrquake-launcher-helpers.c"
LAUNCHER = ROOT / "alpine/aports/fplinux-tyrquake/fplinux-quake.c"
SHARED_INCLUDE = ROOT / "include/fplinux"
EXPECTED_CONTROLS = ROOT / "tests/fixtures/quake/controls.cfg.fixture"


@pytest.fixture(scope="module")
def quake_helpers(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link production resource operations and keep their build tree until teardown."""
    with c_build_directory(tmp_path_factory, "tyrquake-helpers") as directory:
        executable = directory / "cleanup-harness"
        launcher_object = directory / "fplinux-quake.o"
        compile_c(
            launcher_object,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{SHARED_INCLUDE}",
                "-Dmain=fplinux_quake_program_main",
                "-c",
                str(LAUNCHER),
            ],
            name="compile TyrQuake launcher object",
        )
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(HARNESS),
                f"-I{SHARED_INCLUDE}",
                str(launcher_object),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
            ],
            name="link TyrQuake resource harness",
        )
        yield executable


@pytest.fixture(scope="module")
def quake_launcher(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the production entry point without replacing its parser."""
    with c_build_directory(tmp_path_factory, "tyrquake-cli") as directory:
        executable = directory / "quake"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{SHARED_INCLUDE}",
                str(LAUNCHER),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
            ],
            name="compile TyrQuake launcher CLI",
        )
        yield executable


class TyrQuakeLauncherHelperTests:
    """Exercise resource operations without claiming device coverage."""

    def test_remove_runtime_deletes_tree_and_keeps_linked_game_data(
        self, quake_helpers: Path
    ) -> None:
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
                [str(quake_helpers), str(runtime)],
                name="run TyrQuake cleanup harness",
                timeout=10,
                check=True,
            )

            assert not (runtime.exists())
            assert (pak.read_bytes()) == (b"pak\n")

    def test_controls_are_private_and_existing_configuration_is_preserved(
        self, quake_helpers: Path, tmp_path: Path
    ) -> None:
        """Embedded defaults create a private config and refuse an existing file."""
        command = [str(quake_helpers), "write-controls", str(tmp_path)]
        run_process(command, name="create Quake controls", timeout=5, check=True)
        configuration = tmp_path / "config.cfg"
        contents = configuration.read_bytes()
        assert stat.S_IMODE(configuration.stat().st_mode) == 0o600
        assert contents == EXPECTED_CONTROLS.read_bytes()
        configuration.write_bytes(b"existing controls\n")

        result = run_process(command, name="refuse existing Quake controls", timeout=5)

        assert result.returncode != 0
        assert "cannot create control configuration" in result.stderr
        assert configuration.read_bytes() == b"existing controls\n"


class TyrQuakeLauncherCliTests:
    """Run only help and invalid CLI paths of the host-compiled launcher."""

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
    def test_help_exits_without_game_data_or_display(
        self, quake_launcher: Path, arguments: tuple[str, ...]
    ) -> None:
        """Parsed help takes priority over invalid or extra arguments."""
        result = run_process(
            [str(quake_launcher), *arguments],
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
    def test_invalid_syntax_has_no_game_output(
        self, quake_launcher: Path, arguments: tuple[str, ...]
    ) -> None:
        """Unknown options, missing values and extra arguments are refused."""
        result = run_process(
            [str(quake_launcher), *arguments],
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
    def test_heap_size_outside_the_supported_range_is_refused(
        self, quake_launcher: Path, value: str
    ) -> None:
        """A size the engine cannot use is rejected before any device is touched."""
        result = run_process(
            [str(quake_launcher), f"--heapsize={value}"],
            name="run TyrQuake launcher heap error",
            timeout=5,
        )

        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("--heapsize") in (result.stderr)

    def test_help_lists_the_heap_size_and_its_default(self, quake_launcher: Path) -> None:
        """The size is part of the published interface, not a hidden constant."""
        result = run_process(
            [str(quake_launcher), "--help"],
            name="run TyrQuake launcher help",
            timeout=5,
        )

        assert (result.returncode) == (0), result.stderr
        assert ("--heapsize") in (result.stdout)
        assert ("32768") in (result.stdout)
