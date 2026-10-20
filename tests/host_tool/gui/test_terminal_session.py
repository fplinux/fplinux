# SPDX-License-Identifier: GPL-2.0-only
"""Host session I/O and process cleanup through real Linux PTYs."""

from __future__ import annotations

import os
import signal
import time
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.gettext_support import gettext_link_flags
from tests.process import process_state, run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

TERMINAL = ROOT / "alpine/aports/fplinux-terminal"
PEERS = Path(__file__).parent


@dataclass(frozen=True)
class TerminalSessionHarness:
    """Session, separately executed guardian and controlled shell peer."""

    executable: Path
    child: Path


@pytest.fixture(scope="module")
def terminal_session_harness(
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[TerminalSessionHarness]:
    """Link production translation units without replacing PTY ownership."""
    with c_build_directory(tmp_path_factory, "terminal-session") as directory:
        executable = directory / "terminal-session-test"
        guardian = directory / "terminal-pty-supervisor"
        child = directory / "terminal-pty-child"
        compiler = ["-std=c11", "-Wall", "-Wextra", "-Werror"]
        compile_c(
            guardian,
            [*compiler, str(TERMINAL / "terminal-pty-supervisor.c")],
            name="compile real terminal PTY guardian",
        )
        compile_c(
            child,
            [*compiler, str(PEERS / "terminal-pty-child.c")],
            name="compile terminal session shell peer",
        )
        compile_c(
            executable,
            [
                *compiler,
                f'-DFPLINUX_TERMINAL_PTY_HELPER_PATH="{guardian}"',
                "-I",
                str(TERMINAL),
                "-I",
                str(ROOT / "include/fplinux"),
                str(PEERS / "terminal-session-test.c"),
                str(TERMINAL / "terminal-engine.c"),
                str(TERMINAL / "terminal-input.c"),
                str(TERMINAL / "terminal-session.c"),
                str(TERMINAL / "terminal-pty.c"),
                str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                str(ROOT / "lib/fplinux/fplinux-language.c"),
                "-ltsm",
                *gettext_link_flags(),
                "-Wl,--wrap=read",
            ],
            name="link terminal session host harness",
        )
        yield TerminalSessionHarness(executable, child)


def _stop_owned_jobs(directory: Path) -> None:
    """Contain failing lifecycle cases using the recorded, still-matching SID."""
    receipt = directory / "pty-pids"
    if not receipt.exists():
        return
    shell, guardian = (int(value) for value in receipt.read_text().split())
    pids = [shell]
    pids.extend(int(path.read_text()) for path in directory.glob("*.pid"))
    for pid in pids:
        with suppress(ProcessLookupError):
            if os.getsid(pid) == shell:
                os.kill(pid, signal.SIGKILL)
    deadline = time.monotonic() + 1
    while process_state(guardian) not in (None, "Z") and time.monotonic() < deadline:
        time.sleep(0.01)
    with suppress(ProcessLookupError):
        if os.getsid(guardian) == guardian and process_state(guardian) != "Z":
            os.kill(guardian, signal.SIGKILL)


@pytest.mark.parametrize(
    ("scenario", "status"),
    [("success", 0), ("failure", 37), ("buffered-failure", 37), ("signal", 143)],
    ids=["success", "failure", "buffered-output", "signal-status"],
)
def test_session_drains_last_grid_output_and_preserves_shell_status(
    terminal_session_harness: TerminalSessionHarness,
    tmp_path: Path,
    scenario: str,
    status: int,
) -> None:
    result = _run_session(terminal_session_harness, tmp_path, scenario)
    assert result == status


def test_closed_pty_stream_waits_for_actual_shell_failure(
    terminal_session_harness: TerminalSessionHarness, tmp_path: Path
) -> None:
    assert _run_session(terminal_session_harness, tmp_path, "closed-stream") == 0


def test_session_keeps_literal_input_queued_until_pty_accepts_it(
    terminal_session_harness: TerminalSessionHarness, tmp_path: Path
) -> None:
    assert _run_session(terminal_session_harness, tmp_path, "backpressure") == 0


def test_pty_close_terminates_and_reaps_foreground_background_and_disowned_jobs(
    terminal_session_harness: TerminalSessionHarness, tmp_path: Path
) -> None:
    assert _run_session(terminal_session_harness, tmp_path, "jobs") == 0


def test_gui_process_group_death_leaves_no_shell_jobs_or_guardian(
    terminal_session_harness: TerminalSessionHarness, tmp_path: Path
) -> None:
    assert _run_session(terminal_session_harness, tmp_path, "owner-death") == 0


def test_shipped_bashrc_keeps_readline_editing_completion_history_and_prompt_authentication(
    terminal_session_harness: TerminalSessionHarness, tmp_path: Path
) -> None:
    assert _run_session(terminal_session_harness, tmp_path, "bash-readline") == 0


def _run_session(harness: TerminalSessionHarness, directory: Path, scenario: str) -> int:
    environment = {
        **os.environ,
        "HOME": str(directory),
        "HISTFILE": str(directory / ".bash_history"),
        "HISTCONTROL": "",
        "HISTIGNORE": "",
        "INPUTRC": "/dev/null",
        "LC_ALL": "C.UTF-8",
        "TERMINAL_TEST_CHILD": str(harness.child),
        "XDG_STATE_HOME": str(directory / "state"),
    }
    environment.pop("BASH_ENV", None)
    environment.pop("ENV", None)
    try:
        result = run_process(
            [
                str(harness.executable),
                scenario,
                str(harness.child),
                str(PEERS),
                str(TERMINAL / "fplinux-terminal.bashrc"),
            ],
            name=f"run real PTY session {scenario}",
            timeout=15,
            cwd=directory,
            env=environment,
        )
        assert result.returncode >= 0, result.stderr
        assert result.stderr == "", result.stderr
        return result.returncode
    finally:
        _stop_owned_jobs(directory)
