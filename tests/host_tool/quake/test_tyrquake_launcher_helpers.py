# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for TyrQuake launcher arguments and runtime cleanup."""

from __future__ import annotations

import fcntl
import os
import shutil
import signal
import stat
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator

HARNESS = ROOT / "tests/host_tool/quake/fplinux-tyrquake-launcher-helpers.c"
PROCESS_HARNESS = ROOT / "tests/host_tool/quake/fplinux-tyrquake-launcher-process.c"
LAUNCHER = ROOT / "alpine/aports/fplinux-tyrquake/fplinux-quake.c"
SHARED_INCLUDE = ROOT / "include/fplinux"
EXPECTED_CONTROLS = ROOT / "tests/fixtures/quake/controls.cfg.fixture"
RUNTIME_ROOT = Path("/tmp")  # noqa: S108 -- the launcher owns runtime directories here.


@dataclass(frozen=True)
class EngineReport:
    """Values observed by the separately executed engine peer."""

    arguments: tuple[str, ...]
    executable: str
    home: Path
    process_id: int
    launcher_id: int
    lock_closed: bool
    signals_default: bool


def read_engine_report(path: Path) -> EngineReport:
    """Read the peer's complete report without deriving expected engine arguments."""
    lines = path.read_text(encoding="utf-8").splitlines()
    assert lines[-1] == "ready=1"
    values = dict(line.split("=", 1) for line in lines if not line.startswith("arg="))
    return EngineReport(
        arguments=tuple(line.removeprefix("arg=") for line in lines if line.startswith("arg=")),
        executable=values["executable"],
        home=Path(values["home"]),
        process_id=int(values["pid"]),
        launcher_id=int(values["parent"]),
        lock_closed=values["lock_closed"] == "1",
        signals_default=values["signals_default"] == "1",
    )


@pytest.fixture
def quake_engine_environment(tmp_path: Path) -> Iterator[dict[str, str]]:
    """Own fake card data and the lock while leaving process and runtime operations real."""
    (tmp_path / "pak0.pak").write_bytes(b"test-owned game data\n")
    yield {
        "FPLINUX_QUAKE_TEST_ROOT": str(tmp_path),
        "HOME": str(tmp_path / "original-home"),
        "LC_ALL": "C",
    }
    report_path = tmp_path / "engine-report"
    if report_path.exists():
        report = read_engine_report(report_path)
        runtime = RUNTIME_ROOT / f"fplinux-quake.{report.launcher_id}"
        shutil.rmtree(runtime, ignore_errors=True)


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
    """Replace card paths and the engine executable, retaining the real launcher process."""
    with c_build_directory(tmp_path_factory, "tyrquake-cli") as directory:
        executable = directory / "quake"
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
            name="compile TyrQuake launcher process object",
        )
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{SHARED_INCLUDE}",
                str(PROCESS_HARNESS),
                str(launcher_object),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                "-Wl,--wrap=open",
                "-Wl,--wrap=mkdir",
                "-Wl,--wrap=fopen",
                "-Wl,--wrap=stat",
                "-Wl,--wrap=access",
                "-Wl,--wrap=symlink",
                "-Wl,--wrap=execv",
            ],
            name="link TyrQuake launcher process harness",
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
    """Run the production parser before any fake card or engine access."""

    @pytest.mark.parametrize(
        "arguments",
        [
            ("-h",),
            ("--help",),
            ("--heapsize=invalid", "--help"),
            ("--renderer=invalid", "--help"),
            ("--unknown", "--help"),
            ("extra", "-h"),
        ],
        ids=[
            "h",
            "help",
            "heapsize-invalid-help",
            "renderer-invalid-help",
            "unknown-help",
            "extra-h",
        ],
    )
    def test_help_exits_without_game_data_or_display(
        self,
        quake_launcher: Path,
        quake_engine_environment: dict[str, str],
        tmp_path: Path,
        arguments: tuple[str, ...],
    ) -> None:
        """Parsed help takes priority over invalid or extra arguments."""
        result = run_process(
            [str(quake_launcher), *arguments],
            name="run TyrQuake launcher help",
            timeout=5,
            env=quake_engine_environment,
        )

        assert (result.returncode) == (0), result.stderr
        assert ("Usage:") in (result.stdout)
        assert (result.stderr) == ("")
        assert not (tmp_path / "resources-opened").exists()

    @pytest.mark.parametrize(
        "arguments",
        [
            ("--unknown",),
            ("--heapsize",),
            ("--renderer",),
            ("--stats=1",),
            ("extra",),
            ("--heapsize=16384", "extra"),
            ("--", "--help"),
            ("--renderer=software", "--", "+map", "start"),
        ],
        ids=[
            "unknown",
            "heapsize",
            "renderer",
            "stats-value",
            "extra",
            "heapsize-16384-extra",
            "separator-help",
            "renderer-engine-tail",
        ],
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

    @pytest.mark.parametrize("value", ["", "invalid", "GPU", "auto,software", "--help"])
    def test_invalid_renderer_is_refused_before_resources_are_opened(
        self,
        quake_launcher: Path,
        quake_engine_environment: dict[str, str],
        tmp_path: Path,
        value: str,
    ) -> None:
        """A renderer typo returns a CLI error without starting a session."""
        result = run_process(
            [str(quake_launcher), f"--renderer={value}"],
            name="run TyrQuake launcher renderer error",
            timeout=5,
            env=quake_engine_environment,
        )

        assert result.returncode == 2, result.stderr
        assert result.stdout == ""
        assert "--renderer" in result.stderr
        assert not (tmp_path / "resources-opened").exists()
        assert not (tmp_path / "engine-report").exists()

    def test_help_lists_renderer_choices_and_the_auto_default(self, quake_launcher: Path) -> None:
        """Renderer selection is discoverable through the launcher interface."""
        result = run_process(
            [str(quake_launcher), "--help"],
            name="run TyrQuake renderer help",
            timeout=5,
        )

        assert result.returncode == 0, result.stderr
        assert "--renderer" in result.stdout
        assert "auto|software|gpu" in result.stdout
        assert "default auto" in result.stdout
        assert "--stats" in result.stdout


class TyrQuakeLauncherProcessTests:
    """Observe engine handoff and session ownership with host-only card and executable peers."""

    @pytest.mark.parametrize(
        ("arguments", "engine", "fallback", "heap", "stats"),
        [
            ((), "/usr/bin/tyr-glquake", ("-fplinux-software-fallback",), "32768", "0"),
            (
                ("--renderer=auto",),
                "/usr/bin/tyr-glquake",
                ("-fplinux-software-fallback",),
                "32768",
                "0",
            ),
            (("--renderer=gpu",), "/usr/bin/tyr-glquake", (), "32768", "0"),
            (("--renderer=software",), "/usr/bin/tyr-quake", (), "32768", "0"),
            (
                ("--renderer", "software", "--heapsize=16384"),
                "/usr/bin/tyr-quake",
                (),
                "16384",
                "0",
            ),
            (
                ("--stats",),
                "/usr/bin/tyr-glquake",
                ("-fplinux-software-fallback",),
                "32768",
                "1",
            ),
            (("--renderer=software", "--stats"), "/usr/bin/tyr-quake", (), "32768", "1"),
        ],
        ids=[
            "default-auto",
            "explicit-auto",
            "gpu",
            "software",
            "software-custom-heap",
            "auto-stats",
            "software-stats",
        ],
    )
    def test_renderer_handoff_preserves_engine_arguments_home_and_exit_status(  # noqa: PLR0913 -- keep renderer inputs and independent engine expectations explicit.
        self,
        *,
        quake_launcher: Path,
        quake_engine_environment: dict[str, str],
        tmp_path: Path,
        arguments: tuple[str, ...],
        engine: str,
        fallback: tuple[str, ...],
        heap: str,
        stats: str,
    ) -> None:
        """Only auto allows fallback; all modes retain the established engine invocation."""
        result = run_process(
            [str(quake_launcher), *arguments],
            name="run TyrQuake renderer handoff",
            timeout=5,
            env=quake_engine_environment,
        )

        assert result.returncode == 23, result.stderr
        assert result.stdout == ""
        assert result.stderr == ""
        report = read_engine_report(tmp_path / "engine-report")
        assert report.executable == engine
        assert report.arguments == (
            engine,
            "-nolan",
            "-basedir",
            str(report.home),
            "-heapsize",
            heap,
            "+mlook",
            "+fplinux_stats",
            stats,
            *fallback,
        )
        assert report.home == RUNTIME_ROOT / f"fplinux-quake.{report.launcher_id}"
        assert report.home != Path(quake_engine_environment["HOME"])
        assert report.process_id != report.launcher_id
        assert report.lock_closed
        assert report.signals_default
        assert not report.home.exists()
        assert (tmp_path / "pak0.pak").read_bytes() == b"test-owned game data\n"
        with (tmp_path / "session.lock").open("r+") as lock:
            fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_launcher_owns_lock_forwards_signal_and_removes_runtime(
        self,
        quake_launcher: Path,
        quake_engine_environment: dict[str, str],
        tmp_path: Path,
    ) -> None:
        """Signaling the waiting launcher stops its engine and releases the live session."""
        environment = {**quake_engine_environment, "FPLINUX_QUAKE_TEST_WAIT": "1"}
        report_path = tmp_path / "engine-report"

        def inspect_session_and_signal(process: subprocess.Popen[str], deadline: float) -> None:
            while time.monotonic() < deadline:
                if report_path.exists() and report_path.read_text(encoding="utf-8").endswith(
                    "ready=1\n"
                ):
                    break
                assert process.poll() is None, "launcher exited before its engine became ready"
                time.sleep(0.01)
            else:
                pytest.fail("engine peer did not become ready")

            report = read_engine_report(report_path)
            assert report.launcher_id == process.pid
            assert report.lock_closed
            assert report.signals_default
            assert stat.S_IMODE(report.home.stat().st_mode) == 0o700
            assert stat.S_IMODE((report.home / "id1").stat().st_mode) == 0o700
            assert (report.home / "id1/config.cfg").read_bytes() == EXPECTED_CONTROLS.read_bytes()
            assert (report.home / "id1/pak0.pak").resolve() == tmp_path / "pak0.pak"
            with (
                (tmp_path / "session.lock").open("r+") as lock,
                pytest.raises(BlockingIOError),
            ):
                fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            os.kill(process.pid, signal.SIGTERM)

        result = run_process(
            [str(quake_launcher), "--renderer=gpu"],
            name="signal TyrQuake launcher session",
            timeout=5,
            env=environment,
            while_running=inspect_session_and_signal,
        )

        assert result.returncode == 128 + signal.SIGTERM, result.stderr
        assert result.stderr == ""
        report = read_engine_report(report_path)
        assert not report.home.exists()
        assert not Path(f"/proc/{report.process_id}").exists()
        with (tmp_path / "session.lock").open("r+") as lock:
            fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
