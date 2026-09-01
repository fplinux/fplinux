# SPDX-License-Identifier: GPL-2.0-only
"""Host-process evidence for streaming a bound SSH command into a binary file."""

from __future__ import annotations

import importlib.util
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import suppress
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli import ssh_transport

from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]
PLUGIN = ROOT / "targets/nokia-ta1618/profiles/nand-ro-lab/host_plugin.py"


def load_plugin() -> ModuleType:
    """Load the production profile plugin through its normal module entry point."""
    spec = importlib.util.spec_from_file_location("test_nand_ro_lab_plugin", PLUGIN)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load profile plugin: {PLUGIN}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class NandBackupSshStreamTests(unittest.TestCase):
    """Exercise the real SSH process boundary with a controlled local ssh executable."""

    def setUp(self) -> None:
        """Create one valid session and a fake SSH executable per test."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "runtime"
        self.root.mkdir(mode=0o700)
        self.session = create_ready_session(self.root)
        self.tools = Path(self.temporary.name) / "bin"
        self.tools.mkdir()
        self.ssh = self.tools / "ssh"
        self.plugin = load_plugin()
        self.ssh.write_text(
            """#!/bin/sh
case "${FPLINUX_STREAM_MODE:?}" in
success)
  printf 'raw-page-bytes'
  printf 'remote diagnostic' >&2
  ;;
short)
  printf 'short'
  ;;
nonzero)
  printf 'partial'
  printf 'NAND read failed' >&2
  exit 8
  ;;
timeout)
  printf '%s' "$$" >"${FPLINUX_STREAM_PID:?}"
  exec sleep 60
  ;;
*) exit 64 ;;
esac
""",
            encoding="ascii",
        )
        self.ssh.chmod(0o755)

    def _stream(self, mode: str, destination: Path, *, timeout: float) -> None:
        with (
            destination.open("w+b") as stream,
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch.dict(
                os.environ,
                {"PATH": f"{self.tools}:{os.environ['PATH']}", "FPLINUX_STREAM_MODE": mode},
            ),
        ):
            ssh_transport.stream_remote(
                self.session, "exec dd if=/dev/nand", stream, timeout=timeout
            )

    def test_success_stream_keeps_ssh_stderr_out_of_the_binary_destination(self) -> None:
        """Real child stdout goes straight to the file while stderr remains diagnostic-only."""
        destination = Path(self.temporary.name) / "nand.raw"

        self._stream("success", destination, timeout=2)

        self.assertEqual(destination.read_bytes(), b"raw-page-bytes")

    def test_nonzero_ssh_exit_reports_the_remote_diagnostic(self) -> None:
        """A failed remote reader gives its exit status and stderr to the caller."""
        destination = Path(self.temporary.name) / "nand.raw"

        with self.assertRaisesRegex(SystemExit, "exit status 8: NAND read failed"):
            self._stream("nonzero", destination, timeout=2)

        self.assertEqual(destination.read_bytes(), b"partial")

    def test_short_remote_stream_preserves_an_existing_backup(self) -> None:
        """A real SSH short read is rejected before it can replace the previous image."""
        destination = Path(self.temporary.name) / "nand.raw"
        destination.write_bytes(b"previous raw image")
        with (
            mock.patch.object(self.plugin, "RAW_BYTES", 16),
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch.dict(
                os.environ,
                {"PATH": f"{self.tools}:{os.environ['PATH']}", "FPLINUX_STREAM_MODE": "short"},
            ),
            self.assertRaisesRegex(SystemExit, "incomplete raw image"),
        ):
            self.plugin.backup(ssh_transport, self.session, str(destination))

        self.assertEqual(destination.read_bytes(), b"previous raw image")
        self.assertEqual(list(destination.parent.glob(".nand.raw.*")), [])

    def test_timeout_kills_and_reaps_the_isolated_ssh_process_group(self) -> None:
        """A stuck transfer terminates within its deadline without leaving its SSH child alive."""
        destination = Path(self.temporary.name) / "nand.raw"
        pid_file = Path(self.temporary.name) / "ssh.pid"
        with (
            mock.patch.dict(os.environ, {"FPLINUX_STREAM_PID": str(pid_file)}),
            self.assertRaisesRegex(SystemExit, "timed out after 0.2s"),
        ):
            self._stream("timeout", destination, timeout=0.2)

        process_id = int(pid_file.read_text(encoding="ascii"))
        with self.assertRaises(ProcessLookupError):
            os.kill(process_id, 0)

    def test_interrupt_forwards_to_an_isolated_stream_helper_and_reaps_ssh(self) -> None:
        """SIGINT stops the SSH child group without delivering a signal to this test runner."""
        runtime_base = Path(self.temporary.name) / "xdg-runtime"
        runtime_base.mkdir(mode=0o700)
        runtime_root = runtime_base / "fplinux"
        runtime_root.mkdir(mode=0o700)
        session = create_ready_session(runtime_root)
        session_path = Path(session["private_key"]).parent / "session.json"
        destination = Path(self.temporary.name) / "nand.raw"
        child_pid = Path(self.temporary.name) / "ssh.pid"
        helper = Path(self.temporary.name) / "stream-helper.py"
        helper.write_text(
            """import json
import sys
from pathlib import Path

from fplinux_cli import ssh_transport

session = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
with Path(sys.argv[2]).open('w+b') as destination:
    ssh_transport.stream_remote(session, 'exec dd if=/dev/nand', destination, timeout=30)
""",
            encoding="utf-8",
        )
        environment = {
            **os.environ,
            "FPLINUX_STREAM_MODE": "timeout",
            "FPLINUX_STREAM_PID": str(child_pid),
            "PATH": f"{self.tools}:{os.environ['PATH']}",
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONPATH": str(Path(__file__).resolve().parents[2] / "scripts"),
            "XDG_RUNTIME_DIR": str(runtime_base),
        }
        process = subprocess.Popen(
            [sys.executable, str(helper), str(session_path), str(destination)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            deadline = time.monotonic() + 5
            while not child_pid.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(child_pid.exists(), "stream helper did not start its SSH child")
            os.kill(process.pid, signal.SIGINT)
            stdout, stderr = process.communicate(timeout=5)
        except BaseException:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)
            raise

        self.assertEqual(process.returncode, 130, f"stdout:\n{stdout}\nstderr:\n{stderr}")
        process_id = int(child_pid.read_text(encoding="ascii"))
        with self.assertRaises(ProcessLookupError):
            os.kill(process_id, 0)


if __name__ == "__main__":
    unittest.main()
