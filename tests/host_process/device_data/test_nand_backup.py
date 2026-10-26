# SPDX-License-Identifier: GPL-2.0-only
"""Host-process evidence for streaming a bound SSH command into a binary file."""

from __future__ import annotations

import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from contextlib import ExitStack, suppress
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli.runtime import nand_backup, ssh_transport

if TYPE_CHECKING:
    from collections.abc import Iterator

from tests import ROOT
from tests.ssh_transport_support import create_ready_session

SSH_FIXTURES = ROOT / "tests" / "fixtures" / "ssh_transport"

# Reader reports for the two fitted chips, in the kernel's key=value format.
GEOMETRY_FIXTURES = ROOT / "tests/fixtures/nand-geometry"
GEOMETRY_128_OOB = (GEOMETRY_FIXTURES / "demo-128.txt").read_text(encoding="utf-8")
GEOMETRY_64_OOB = (GEOMETRY_FIXTURES / "demo-64.txt").read_text(encoding="utf-8")


@dataclass(frozen=True)
class NandSshContext:
    """A controlled session and SSH executable within one disposable directory."""

    directory: Path
    root: Path
    session: dict[str, Any]
    tools: Path

    def stream(self, mode: str, destination: Path, *, timeout: float) -> None:
        """Execute the real stream transport with the fake SSH process."""
        with (
            destination.open("w+b") as stream,
            mock.patch.dict(
                os.environ,
                {"PATH": f"{self.tools}:{os.environ['PATH']}", "FPLINUX_STREAM_MODE": mode},
            ),
        ):
            ssh_transport.stream_remote(
                self.session, "exec dd if=/dev/nand", stream, timeout=timeout
            )


@pytest.fixture
def nand_ssh_case() -> Iterator[NandSshContext]:
    """Remove the runtime keys and restore the reporter root after every test."""
    with ExitStack() as cleanup:
        temporary = cleanup.enter_context(tempfile.TemporaryDirectory())
        directory = Path(temporary)
        cleanup.enter_context(mock.patch("fplinux_cli.reporting.run.ROOT", directory))
        root = directory / "runtime"
        root.mkdir(mode=0o700)
        session = create_ready_session(root)
        tools = directory / "bin"
        tools.mkdir()
        ssh = tools / "ssh"
        shutil.copyfile(SSH_FIXTURES / "nand_ssh.sh", ssh)
        ssh.chmod(0o755)
        yield NandSshContext(directory, root, session, tools)


class NandBackupSshStreamTests:
    """Exercise the real SSH process boundary with a controlled local ssh executable."""

    def test_success_stream_keeps_ssh_stderr_out_of_the_binary_destination(
        self, nand_ssh_case: NandSshContext
    ) -> None:
        """Real child stdout goes straight to the file while stderr remains diagnostic-only."""
        destination = nand_ssh_case.directory / "nand.raw"

        nand_ssh_case.stream("success", destination, timeout=2)

        assert (destination.read_bytes()) == (b"raw-page-bytes")

    def test_nonzero_ssh_exit_reports_the_remote_diagnostic(
        self, nand_ssh_case: NandSshContext
    ) -> None:
        """A failed remote reader gives its exit status and stderr to the caller."""
        destination = nand_ssh_case.directory / "nand.raw"

        with pytest.raises(SystemExit, match="exit status 8: NAND read failed"):
            nand_ssh_case.stream("nonzero", destination, timeout=2)

        assert (destination.read_bytes()) == (b"partial")

    def test_short_remote_stream_preserves_an_existing_backup(
        self, nand_ssh_case: NandSshContext
    ) -> None:
        """A real SSH short read is rejected before it can replace the previous image."""
        destination = nand_ssh_case.directory / "nand.raw"
        destination.write_bytes(b"previous raw image")
        with (
            mock.patch.dict(
                os.environ,
                {
                    "PATH": f"{nand_ssh_case.tools}:{os.environ['PATH']}",
                    "FPLINUX_STREAM_MODE": "short",
                    "FPLINUX_GEOMETRY": GEOMETRY_128_OOB,
                },
            ),
            pytest.raises(SystemExit, match="incomplete raw NAND image"),
        ):
            nand_backup.backup_nand(
                lambda: (ssh_transport, nand_ssh_case.session),
                destination,
                target="nokia-ta1618",
                acquisition_recipe="a" * 64,
                raw_device="/dev/ums9117-nand-raw",
            )

        assert (destination.read_bytes()) == (b"previous raw image")
        assert (list(destination.parent.glob(".nand.raw.*"))) == ([])

    def test_backup_streams_only_the_requested_read_device(
        self, nand_ssh_case: NandSshContext
    ) -> None:
        """The shared backup reaches the real SSH process with one read of its selected device."""
        destination = nand_ssh_case.directory / "nokia.raw"
        commands = nand_ssh_case.directory / "remote-commands"
        with (
            mock.patch.dict(
                os.environ,
                {
                    "PATH": f"{nand_ssh_case.tools}:{os.environ['PATH']}",
                    "FPLINUX_STREAM_MODE": "success",
                    "FPLINUX_REMOTE_COMMANDS": str(commands),
                    "FPLINUX_GEOMETRY": GEOMETRY_128_OOB,
                },
            ),
            pytest.raises(SystemExit, match="expected 142606336 bytes, got 14"),
        ):
            nand_backup.backup_nand(
                lambda: (ssh_transport, nand_ssh_case.session),
                destination,
                target="nokia-ta1618",
                acquisition_recipe="a" * 64,
                raw_device="/dev/ums9117-nand-raw",
            )

        assert not (destination.exists())
        read_commands = commands.read_text(encoding="ascii").splitlines()
        assert (len(read_commands)) == (1), read_commands
        # dd copies the device to stdout; any block size is a valid read.
        assert (
            re.search(r"\Aexec dd if=/dev/ums9117-nand-raw bs=[1-9][0-9]*\Z", read_commands[0])
            is not None
        )

    def test_rejected_device_read_cannot_publish_or_replace_an_image(
        self, nand_ssh_case: NandSshContext
    ) -> None:
        """A remote reader error leaves an existing INOI backup intact."""
        destination = nand_ssh_case.directory / "inoi.raw"
        destination.write_bytes(b"previous complete image")
        with (
            mock.patch.dict(
                os.environ,
                {
                    "PATH": f"{nand_ssh_case.tools}:{os.environ['PATH']}",
                    "FPLINUX_STREAM_MODE": "nonzero",
                    "FPLINUX_GEOMETRY": GEOMETRY_64_OOB,
                },
            ),
            pytest.raises(SystemExit, match="exit status 8: NAND read failed"),
        ):
            nand_backup.backup_nand(
                lambda: (ssh_transport, nand_ssh_case.session),
                destination,
                target="inoi-244-modern-4g",
                acquisition_recipe="a" * 64,
                raw_device="/dev/ums9117-nand-raw",
            )

        assert (destination.read_bytes()) == (b"previous complete image")
        assert (list(destination.parent.glob(".inoi.raw.*"))) == ([])

    def test_timeout_kills_and_reaps_the_isolated_ssh_process_group(
        self, nand_ssh_case: NandSshContext
    ) -> None:
        """A stuck transfer terminates within its deadline without leaving its SSH child alive."""
        destination = nand_ssh_case.directory / "nand.raw"
        pid_file = nand_ssh_case.directory / "ssh.pid"
        with (
            mock.patch.dict(os.environ, {"FPLINUX_STREAM_PID": str(pid_file)}),
            pytest.raises(SystemExit, match=r"timed out after 0.2s"),
        ):
            nand_ssh_case.stream("timeout", destination, timeout=0.2)

        process_id = int(pid_file.read_text(encoding="ascii"))
        with pytest.raises(ProcessLookupError):
            os.kill(process_id, 0)

    def test_interrupt_forwards_to_an_isolated_stream_helper_and_reaps_ssh(
        self, nand_ssh_case: NandSshContext
    ) -> None:
        """SIGINT stops the SSH child group without delivering a signal to this test runner."""
        runtime_root = nand_ssh_case.directory / "stream-runtime"
        runtime_root.mkdir(mode=0o700)
        session = create_ready_session(runtime_root)
        session_path = Path(session["private_key"]).parent / "session.json"
        destination = nand_ssh_case.directory / "nand.raw"
        child_pid = nand_ssh_case.directory / "ssh.pid"
        helper = nand_ssh_case.directory / "stream-helper.py"
        shutil.copyfile(SSH_FIXTURES / "stream_helper.py", helper)
        environment = {
            **os.environ,
            "FPLINUX_STREAM_MODE": "timeout",
            "FPLINUX_STREAM_PID": str(child_pid),
            "PATH": f"{nand_ssh_case.tools}:{os.environ['PATH']}",
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONPATH": str(ROOT / "scripts"),
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
            assert child_pid.exists(), "stream helper did not start its SSH child"
            os.kill(process.pid, signal.SIGINT)
            stdout, stderr = process.communicate(timeout=5)
        except BaseException:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)
            raise

        assert (process.returncode) == (130), f"stdout:\n{stdout}\nstderr:\n{stderr}"
        process_id = int(child_pid.read_text(encoding="ascii"))
        with pytest.raises(ProcessLookupError):
            os.kill(process_id, 0)
