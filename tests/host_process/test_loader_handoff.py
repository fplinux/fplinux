# SPDX-License-Identifier: GPL-2.0-only
"""Loader event and shell lifetime with local RAM-tool, USB and SSH doubles."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any

from tests.process import python_environment, run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]


class LoaderHandoffProcessTests(unittest.TestCase):
    """Exercise real process replacement without qualifying a phone or SSH server."""

    def setUp(self) -> None:
        """Own all bundle, event and private session files for one scenario."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

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

    def test_interactive_shell_observes_complete_before_replacing_runner(self) -> None:
        """An interactive exec preserves the completed loader stream and SSH exit status."""
        result = self.run_loader("interactive")
        self.assertEqual(result.returncode, 23, result.stderr)
        shell = json.loads((self.root / "shell.json").read_text(encoding="utf-8"))
        self.assertEqual(shell["pid"], int((self.root / "runner.pid").read_text(encoding="ascii")))
        self.assertEqual(
            [event["event"] for event in shell["events"]],
            [
                "waiting-for-device",
                "ram-loader-complete",
                "linux-transition",
                "linux-usb",
                "ssh-ready",
                "complete",
            ],
        )
        self.assertEqual(self.events(), shell["events"])
        self.assertEqual(self.commands()[0], "uname -r")
        self.assertTrue(self.commands()[1].startswith("fplinux-clock "))
        self.assertEqual(self.commands()[2], "root@10.23.45.2")
        self.assertEqual(len(list(self.root.glob("runtime/fplinux/sessions/*/client_ed25519"))), 1)
        self.assertEqual(list(self.root.glob("runtime/fplinux/sessions/*/ramboot.bin")), [])

    def test_noninteractive_handoff_completes_once_without_starting_shell(self) -> None:
        """The ready session credentials remain after the loader returns normally."""
        result = self.run_loader("noninteractive")
        self.assertEqual(result.returncode, 0, result.stderr)
        names = [event["event"] for event in self.events()]
        self.assertEqual(names[-2:], ["ssh-ready", "complete"])
        self.assertEqual(names.count("complete"), 1)
        self.assertFalse((self.root / "shell.json").exists())
        self.assertEqual(len(self.commands()), 2)
        self.assertIn("No interactive terminal is attached", result.stdout)
        self.assertEqual(len(list(self.root.glob("runtime/fplinux/sessions/*/client_ed25519"))), 1)

    def test_none_transport_completes_without_ssh_and_removes_unready_session(self) -> None:
        """A declared no-transport load ends at the acknowledged Linux transition."""
        result = self.run_loader("none")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            [event["event"] for event in self.events()],
            ["waiting-for-device", "ram-loader-complete", "linux-transition", "complete"],
        )
        self.assertEqual(self.commands(), [])
        self.assertEqual(list(self.root.glob("runtime/fplinux/sessions/*")), [])

    def test_unverified_identity_never_completes_or_opens_shell(self) -> None:
        """Authentication or kernel identity failure cannot become a successful load."""
        for mode in ("authentication-failure", "identity-failure"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                original_root = self.root
                self.root = Path(temporary)
                try:
                    result = self.run_loader(mode)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    events = self.events()
                    self.assertEqual(events[-1]["event"], "failure")
                    self.assertEqual(events[-1]["stage"], "linux-usb")
                    self.assertEqual(events[-1]["exit_status"], 1)
                    self.assertNotIn("complete", [event["event"] for event in events])
                    self.assertNotIn("ssh-ready", [event["event"] for event in events])
                    self.assertFalse((self.root / "shell.json").exists())
                    self.assertEqual(
                        self.commands(), [] if mode == "authentication-failure" else ["uname -r"]
                    )
                finally:
                    self.root = original_root


if __name__ == "__main__":
    unittest.main()
