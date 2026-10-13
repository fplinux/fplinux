# SPDX-License-Identifier: GPL-2.0-only
"""Host-process tests for SSH transport with controlled external tools."""

from __future__ import annotations

import base64
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli.runtime import ssh_transport

if TYPE_CHECKING:
    from collections.abc import Iterator

from tests import ROOT
from tests.process import python_environment
from tests.ssh_transport_support import create_ready_session

SSH_FIXTURES = ROOT / "tests" / "fixtures" / "ssh_transport"


@pytest.mark.parametrize(
    ("probe", "capture_output"),
    [("ram-session", False), ("kernel-identity", False), ("both", True)],
    ids=["ram-session", "kernel-identity", "combined-captured-output"],
)
def test_identity_probes_preserve_binary_stdin_for_remote_command(
    tmp_path: Path, probe: str, *, capture_output: bool
) -> None:
    """Real children read fd0; USB/network discovery and the SSH peer are controlled."""
    runtime = tmp_path / "runtime"
    runtime.mkdir(mode=0o700)
    session = create_ready_session(runtime)
    session["runtime_root"] = str(runtime)
    state_path = tmp_path / "session.json"
    state_path.write_text(json.dumps(session), encoding="utf-8")
    output = tmp_path / "received.bin"
    payload = bytes(range(256)) * 256

    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "tests.fixtures.ssh_transport.linux_stdin_consumer",
            str(state_path),
            probe,
            "capture" if capture_output else "inherit",
        ],
        input=payload,
        capture_output=True,
        cwd=ROOT,
        env={
            **python_environment(),
            "FPLINUX_TEST_SESSION_ID": session["session_id"],
            "FPLINUX_TEST_KERNEL": "6.12.0-fplinux-1212121212121212",
            "FPLINUX_TEST_STDIN_OUTPUT": str(output),
        },
        timeout=10,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode(errors="replace")
    assert result.stdout == b"stdin received\n"
    assert output.read_bytes() == payload


@pytest.fixture
def private_runtime() -> Iterator[Path]:
    """Own the private runtime tree used by controlled SSH session inputs."""
    with ExitStack() as cleanup:
        temporary = tempfile.TemporaryDirectory()
        cleanup.enter_context(temporary)
        root = Path(temporary.name) / "runtime"
        root.mkdir(mode=0o700)
        yield root


class SshTransportFakeToolTests:
    """Exercise external-tool boundaries with deterministic local programs."""

    @pytest.mark.parametrize(
        ("address_output", "route_output", "ready"),
        [
            (
                '[{"addr_info":[{"family":"inet","local":"10.23.45.1","prefixlen":30}]}]',
                '[{"dst":"10.23.45.0/30","dev":"usb0"}]',
                True,
            ),
            (
                '[{"addr_info":[{"family":"inet","local":"10.23.45.1","prefixlen":30}]}]',
                '[{"dst":"10.23.45.0/30","dev":"usb1"}]',
                False,
            ),
            (
                '[{"addr_info":[{"family":"inet","local":"10.23.45.1","prefixlen":24}]}]',
                '[{"dst":"10.23.45.0/30","dev":"usb0"}]',
                False,
            ),
        ],
        ids=("connected", "route-through-another-link", "address-in-a-wider-network"),
    )
    def test_network_ready_requires_session_address_and_route_on_its_interface(
        self, private_runtime: Path, *, address_output: str, route_output: str, ready: bool
    ) -> None:
        """Only the session's /30 host address and a route through the same link are ready."""
        tool_directory = private_runtime.parent / "bin"
        tool_directory.mkdir()
        ip_tool = tool_directory / "ip"
        shutil.copyfile(SSH_FIXTURES / "ip.sh", ip_tool)
        ip_tool.chmod(0o755)
        session = create_ready_session(private_runtime)
        with mock.patch.dict(
            os.environ,
            {
                "PATH": str(tool_directory),
                "FPLINUX_IP_ADDRESS": address_output,
                "FPLINUX_IP_ROUTE": route_output,
            },
        ):
            # The local IP-tool stub leaves the transport's readiness decision real.
            readiness = ssh_transport._network_ready("usb0", session)  # noqa: SLF001
            assert readiness == ready

    def test_reacquire_bounds_an_unresponsive_identity_command(
        self, private_runtime: Path
    ) -> None:
        """A stalled SSH probe cannot outlive the session reconnect deadline."""
        session = create_ready_session(private_runtime)
        # A sleeping local process replaces an SSH channel that never replies.
        # USB discovery is controlled; the process timeout and cleanup are real.
        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=private_runtime),
            mock.patch.object(ssh_transport, "_usb_devices", return_value=[Path("/usb/phone")]),
            mock.patch.object(ssh_transport, "_ncm_interface", return_value="usb0"),
            mock.patch.object(ssh_transport, "_network_ready", return_value=True),
            mock.patch.object(
                ssh_transport,
                "_ssh_argv",
                return_value=[sys.executable, "-c", "import time; time.sleep(5)"],
            ),
        ):
            started = time.monotonic()
            with pytest.raises(SystemExit, match="did not reconnect before the deadline"):
                ssh_transport.reacquire_bound_session(session)
            assert (time.monotonic() - started) < (3)

    def test_initial_binding_retries_fake_ssh_tools_and_enforces_key_policy(
        self, private_runtime: Path
    ) -> None:
        """Retry deterministic keyscan output before accepting a matching session id."""
        state = create_ready_session(private_runtime, status="prepared")
        Path(state["known_hosts"]).unlink()
        tool_directory = private_runtime.parent / "ssh-tools"
        tool_directory.mkdir()
        scan_count = tool_directory / "scan-count"
        ssh_arguments = tool_directory / "ssh-arguments"
        host_blob = b"\0\0\0\x0bssh-ed25519\0\0\0\x20" + bytes(range(32))
        host_key = base64.b64encode(host_blob).decode("ascii")
        keyscan = tool_directory / "ssh-keyscan"
        shutil.copyfile(SSH_FIXTURES / "keyscan.sh", keyscan)
        ssh = tool_directory / "ssh"
        shutil.copyfile(SSH_FIXTURES / "identity_ssh.sh", ssh)
        keyscan.chmod(0o755)
        ssh.chmod(0o755)

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=private_runtime),
            mock.patch.object(ssh_transport, "_usb_devices", return_value=[Path("/usb/phone")]),
            mock.patch.object(ssh_transport, "_ncm_interface", return_value="usb0"),
            mock.patch.object(ssh_transport, "_network_ready", return_value=True),
            mock.patch.object(ssh_transport, "_retry_pause"),
            mock.patch.dict(
                os.environ,
                {
                    "PATH": f"{tool_directory}:{os.environ.get('PATH', '')}",
                    "FPLINUX_KEYSCAN_COUNT": str(scan_count),
                    "FPLINUX_HOST_KEY": host_key,
                    "FPLINUX_SSH_ARGUMENTS": str(ssh_arguments),
                    "FPLINUX_SESSION_ID": state["session_id"],
                },
            ),
            mock.patch("builtins.print"),
        ):
            ready = ssh_transport.wait_for_bound_session(state)

        assert (ready["status"]) == ("ready")
        assert (scan_count.read_text(encoding="ascii")) == ("2\n")
        assert (host_key) in (Path(state["known_hosts"]).read_text(encoding="ascii"))
        arguments = set(ssh_arguments.read_text(encoding="ascii").splitlines())
        assert {
            "StrictHostKeyChecking=yes",
            "PasswordAuthentication=no",
            "KbdInteractiveAuthentication=no",
            "ClearAllForwardings=yes",
            "ProxyCommand=none",
            f"BindAddress={state['host_address']}",
        } <= arguments


class SshTransportUploadTests:
    """Run upload shell commands locally with controlled filesystem statistics."""

    @pytest.mark.parametrize(
        ("available_blocks", "directory_exists", "succeeds"),
        [(581, True, True), (17, True, True), (16, True, False), (581, False, False)],
        ids=["ram-root", "reserve-exact", "reserve-short", "missing-directory"],
    )
    def test_upload_checks_capacity_without_a_df_mount_entry(
        self, *, available_blocks: int, directory_exists: bool, succeeds: bool
    ) -> None:
        """Publish with sufficient space; preserve the destination otherwise."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = root / "runtime"
            runtime.mkdir(mode=0o700)
            session = create_ready_session(runtime)
            source = root / "source"
            payload = b"x" * 4096
            source.write_bytes(payload)
            directory = root / "destination"
            destination = directory / "config"
            if directory_exists:
                directory.mkdir()
                destination.write_bytes(b"previous contents")

            # SSH executes a local shell; only df/stat reports are synthetic.
            # Hashing, temporary-file publication and cleanup use real files.
            shell_tools = (SSH_FIXTURES / "filesystem_statistics.sh").read_text(encoding="ascii")

            def local_ssh(
                _session: dict[str, Any],
                command: str,
                shell_prefix: str = shell_tools,
                **_options: object,
            ) -> list[str]:
                return ["/bin/sh", "-c", shell_prefix + command]

            def local_sftp(
                _session: dict[str, Any], command: str
            ) -> subprocess.CompletedProcess[str]:
                operation, local_name, remote_name = shlex.split(command)
                assert (operation) == ("put")
                shutil.copyfile(local_name, remote_name)
                return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

            with (
                mock.patch.object(ssh_transport, "_runtime_root", return_value=runtime),
                mock.patch.object(ssh_transport, "_ssh_argv", side_effect=local_ssh),
                mock.patch.object(ssh_transport, "_sftp", side_effect=local_sftp),
                mock.patch.dict(os.environ, {"FPLINUX_AVAILABLE_BLOCKS": str(available_blocks)}),
                mock.patch("builtins.print"),
            ):
                if succeeds:
                    ssh_transport.upload(session, str(source), str(destination))
                    assert (destination.read_bytes()) == (payload)
                else:
                    with pytest.raises(SystemExit, match="not have enough free space"):
                        ssh_transport.upload(session, str(source), str(destination))
                    if directory_exists:
                        assert (destination.read_bytes()) == (b"previous contents")
                    else:
                        assert not (directory.exists())
            assert (list(directory.glob(".fplinux-upload.*"))) == ([])
