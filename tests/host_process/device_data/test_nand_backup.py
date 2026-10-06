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
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.runtime import nand_backup, ssh_transport

if TYPE_CHECKING:
    from collections.abc import Iterator

from tests import ROOT
from tests.ssh_transport_support import create_ready_session

SSH_FIXTURES = ROOT / "tests" / "fixtures" / "ssh_transport"

# Reader reports for the two fitted chips, in the kernel's key=value format.
GEOMETRY_128_OOB = (
    "id_bytes=a1b1\nchip=demo-128\npage_main_bytes=2048\noob_bytes=128\n"
    "pages_per_block=64\nblock_count=1024\nraw_bytes=142606336\ngeometry_source=table\n"
    "feature_a0=0x00000000\nfeature_b0=0x00000000\nfeature_c0=0x00000000\n"
)
GEOMETRY_64_OOB = (
    "id_bytes=e521\nchip=demo-64\npage_main_bytes=2048\noob_bytes=64\n"
    "pages_per_block=64\nblock_count=1024\nraw_bytes=138412032\ngeometry_source=table\n"
    "feature_a0=0x00000000\nfeature_b0=0x00000010\nfeature_c0=0x00000000\n"
)


class NandBackupSshStreamTests:
    """Exercise the real SSH process boundary with a controlled local ssh executable."""

    @pytest.fixture(autouse=True)
    def _prepare_case(self) -> Iterator[None]:
        """Create one valid session and a fake SSH executable per test."""
        with ExitStack() as cleanup:
            self.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(self.temporary)
            reporting_root = mock.patch(
                "fplinux_cli.reporting.run.ROOT", Path(self.temporary.name)
            )
            cleanup.enter_context(reporting_root)
            self.root = Path(self.temporary.name) / "runtime"
            self.root.mkdir(mode=0o700)
            self.session = create_ready_session(self.root)
            self.tools = Path(self.temporary.name) / "bin"
            self.tools.mkdir()
            self.ssh = self.tools / "ssh"
            shutil.copyfile(SSH_FIXTURES / "nand_ssh.sh", self.ssh)
            self.ssh.chmod(0o755)
            yield

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

        assert (destination.read_bytes()) == (b"raw-page-bytes")

    def test_nonzero_ssh_exit_reports_the_remote_diagnostic(self) -> None:
        """A failed remote reader gives its exit status and stderr to the caller."""
        destination = Path(self.temporary.name) / "nand.raw"

        with pytest.raises(SystemExit, match="exit status 8: NAND read failed"):
            self._stream("nonzero", destination, timeout=2)

        assert (destination.read_bytes()) == (b"partial")

    def test_short_remote_stream_preserves_an_existing_backup(self) -> None:
        """A real SSH short read is rejected before it can replace the previous image."""
        destination = Path(self.temporary.name) / "nand.raw"
        destination.write_bytes(b"previous raw image")
        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch.dict(
                os.environ,
                {
                    "PATH": f"{self.tools}:{os.environ['PATH']}",
                    "FPLINUX_STREAM_MODE": "short",
                    "FPLINUX_GEOMETRY": GEOMETRY_128_OOB,
                },
            ),
            pytest.raises(SystemExit, match="incomplete raw NAND image"),
        ):
            nand_backup.backup_nand(
                lambda: (ssh_transport, self.session),
                destination,
                target="nokia-ta1618",
                raw_device="/dev/ums9117-nand-raw",
            )

        assert (destination.read_bytes()) == (b"previous raw image")
        assert (list(destination.parent.glob(".nand.raw.*"))) == ([])

    def test_target_backup_streams_only_its_declared_read_device(self) -> None:
        """Target selection reaches the real SSH process with one read of its declared device."""
        destination = Path(self.temporary.name) / "nokia.raw"
        commands = Path(self.temporary.name) / "remote-commands"
        with (
            mock.patch(
                "fplinux_cli.runtime.bundle_session.current_target_ssh_session",
                return_value=(ssh_transport, self.session),
            ) as acquire,
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch.dict(
                os.environ,
                {
                    "PATH": f"{self.tools}:{os.environ['PATH']}",
                    "FPLINUX_STREAM_MODE": "success",
                    "FPLINUX_REMOTE_COMMANDS": str(commands),
                    "FPLINUX_GEOMETRY": GEOMETRY_128_OOB,
                },
            ),
            pytest.raises(SystemExit, match="expected 142606336 bytes, got 14"),
        ):
            nand_backup.backup_target_nand("nokia-ta1618", destination, profile="microsd-uboot")

        assert not (destination.exists())
        acquire.assert_called_once_with(
            "nokia-ta1618", profile="microsd-uboot", build_type="release"
        )
        read_commands = commands.read_text(encoding="ascii").splitlines()
        assert (len(read_commands)) == (1), read_commands
        # dd copies the device to stdout; any block size is a valid read.
        assert (
            re.search(r"\Aexec dd if=/dev/ums9117-nand-raw bs=[1-9][0-9]*\Z", read_commands[0])
            is not None
        )

    def test_rejected_device_read_cannot_publish_or_replace_an_image(self) -> None:
        """A remote reader error leaves an existing INOI backup intact."""
        destination = Path(self.temporary.name) / "inoi.raw"
        destination.write_bytes(b"previous complete image")
        with (
            mock.patch(
                "fplinux_cli.runtime.bundle_session.current_target_ssh_session",
                return_value=(ssh_transport, self.session),
            ),
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch.dict(
                os.environ,
                {
                    "PATH": f"{self.tools}:{os.environ['PATH']}",
                    "FPLINUX_STREAM_MODE": "nonzero",
                    "FPLINUX_GEOMETRY": GEOMETRY_64_OOB,
                },
            ),
            pytest.raises(SystemExit, match="exit status 8: NAND read failed"),
        ):
            nand_backup.backup_target_nand("inoi-244-modern-4g", destination)

        assert (destination.read_bytes()) == (b"previous complete image")
        assert (list(destination.parent.glob(".inoi.raw.*"))) == ([])

    def test_timeout_kills_and_reaps_the_isolated_ssh_process_group(self) -> None:
        """A stuck transfer terminates within its deadline without leaving its SSH child alive."""
        destination = Path(self.temporary.name) / "nand.raw"
        pid_file = Path(self.temporary.name) / "ssh.pid"
        with (
            mock.patch.dict(os.environ, {"FPLINUX_STREAM_PID": str(pid_file)}),
            pytest.raises(SystemExit, match=r"timed out after 0.2s"),
        ):
            self._stream("timeout", destination, timeout=0.2)

        process_id = int(pid_file.read_text(encoding="ascii"))
        with pytest.raises(ProcessLookupError):
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
        shutil.copyfile(SSH_FIXTURES / "stream_helper.py", helper)
        environment = {
            **os.environ,
            "FPLINUX_STREAM_MODE": "timeout",
            "FPLINUX_STREAM_PID": str(child_pid),
            "PATH": f"{self.tools}:{os.environ['PATH']}",
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONPATH": str(ROOT / "scripts"),
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
