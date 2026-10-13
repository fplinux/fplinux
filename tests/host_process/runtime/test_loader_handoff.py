# SPDX-License-Identifier: GPL-2.0-only
"""Loader event and shell lifetime with local RAM-tool, USB and SSH doubles."""

from __future__ import annotations

import json
import sys
import tempfile
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

import pytest

from tests import ROOT
from tests.process import python_environment, run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator


@dataclass(frozen=True)
class LoaderHandoff:
    """Bundle, event and private session files owned by one loader invocation."""

    root: Path

    def run_loader(self, mode: str) -> subprocess.CompletedProcess[str]:
        """Run the fixture through production runner and adapter entry points."""
        return run_process(
            [
                sys.executable,
                "-m",
                "tests.fixtures.processes.loader_handoff",
                str(self.root),
                mode,
            ],
            name="loader handoff process",
            cwd=ROOT,
            env=python_environment(),
            timeout=15,
        )

    def events(self) -> list[dict[str, Any]]:
        """Read the consumer-visible JSONL records after one bounded invocation."""
        return [
            json.loads(line)
            for line in (self.root / "events.jsonl").read_text(encoding="utf-8").splitlines()
        ]

    def commands(self) -> list[str]:
        """Read commands seen by the separate SSH executable."""
        path = self.root / "ssh-commands.txt"
        return path.read_text(encoding="utf-8").splitlines() if path.exists() else []


@pytest.fixture
def loader_handoff() -> Iterator[LoaderHandoff]:
    """Keep every invocation's bundle and session tree private until teardown."""
    with ExitStack() as cleanup:
        temporary = tempfile.TemporaryDirectory()
        cleanup.enter_context(temporary)
        yield LoaderHandoff(Path(temporary.name))


class LoaderHandoffProcessTests:
    """Exercise real process replacement without qualifying a phone or SSH server."""

    def test_interactive_shell_observes_complete_before_replacing_runner(
        self, loader_handoff: LoaderHandoff
    ) -> None:
        """An interactive exec preserves the completed loader stream and SSH exit status."""
        result = loader_handoff.run_loader("interactive")
        assert (result.returncode) == (23), result.stderr
        shell = json.loads((loader_handoff.root / "shell.json").read_text(encoding="utf-8"))
        assert (shell["pid"]) == (
            int((loader_handoff.root / "runner.pid").read_text(encoding="ascii"))
        )
        assert ([event["event"] for event in shell["events"]]) == (
            [
                "waiting-for-device",
                "device-found",
                "ram-loader-start",
                "ram-loader-complete",
                "linux-start",
                "linux-transition",
                "linux-usb",
                "ssh-ready",
                "complete",
            ]
        )
        assert (loader_handoff.events()) == (shell["events"])
        assert (loader_handoff.commands()[0]) == ("uname -r")
        assert loader_handoff.commands()[1].startswith("fplinux-clock ")
        assert (loader_handoff.commands()[2]) == ("root@10.23.45.2")
        assert (
            len(list(loader_handoff.root.glob("runtime/fplinux/sessions/*/client_ed25519")))
        ) == (1)
        assert (list(loader_handoff.root.glob("runtime/fplinux/sessions/*/ramboot.bin"))) == ([])

    def test_noninteractive_handoff_completes_once_without_starting_shell(
        self, loader_handoff: LoaderHandoff
    ) -> None:
        """The ready session credentials remain after the loader returns normally."""
        result = loader_handoff.run_loader("noninteractive")
        assert (result.returncode) == (0), result.stderr
        names = [event["event"] for event in loader_handoff.events()]
        assert (names[-2:]) == (["ssh-ready", "complete"])
        assert (names.count("complete")) == (1)
        assert not ((loader_handoff.root / "shell.json").exists())
        assert (len(loader_handoff.commands())) == (2)
        assert ("No interactive terminal is attached") in (result.stdout)
        assert (
            len(list(loader_handoff.root.glob("runtime/fplinux/sessions/*/client_ed25519")))
        ) == (1)

    def test_none_transport_completes_without_ssh_and_removes_unready_session(
        self, loader_handoff: LoaderHandoff
    ) -> None:
        """A declared no-transport load ends at the acknowledged Linux transition."""
        result = loader_handoff.run_loader("none")
        assert (result.returncode) == (0), result.stderr
        assert ([event["event"] for event in loader_handoff.events()]) == (
            [
                "waiting-for-device",
                "device-found",
                "ram-loader-start",
                "ram-loader-complete",
                "linux-start",
                "linux-transition",
                "complete",
            ]
        )
        assert (loader_handoff.commands()) == ([])
        assert (list(loader_handoff.root.glob("runtime/fplinux/sessions/*"))) == ([])

    @pytest.mark.parametrize(
        "mode",
        ["authentication-failure", "identity-failure"],
        ids=["authentication-failure", "identity-failure"],
    )
    def test_unverified_identity_never_completes_or_opens_shell(
        self, loader_handoff: LoaderHandoff, mode: str
    ) -> None:
        """Authentication or kernel identity failure cannot become a successful load."""
        result = loader_handoff.run_loader(mode)
        assert (result.returncode) == (1), result.stderr
        events = loader_handoff.events()
        assert (events[-1]["event"]) == ("failure")
        assert (events[-1]["stage"]) == ("linux-usb")
        assert (events[-1]["exit_status"]) == (1)
        assert ("complete") not in ([event["event"] for event in events])
        assert ("ssh-ready") not in ([event["event"] for event in events])
        assert not ((loader_handoff.root / "shell.json").exists())
        assert (loader_handoff.commands()) == (
            [] if mode == "authentication-failure" else ["uname -r"]
        )
