# SPDX-License-Identifier: GPL-2.0-only
"""Host-tool test for the generated OpenSSH configuration."""

from __future__ import annotations

import fcntl
import shutil
import subprocess
import tempfile
import time
from contextlib import ExitStack
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli.runtime import ssh_transport

from tests.host_tool.runtime.openssh_server import OpenSshServer
from tests.process import process_state, run_process
from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator

_SSH_CONFIG_TIMEOUT_SECONDS = 10
_IDLE_EXPIRY_TIMEOUT_SECONDS = 5


class SshTransportOpenSshConfigTests:
    """Validate generated session configuration with the installed OpenSSH parser."""

    @pytest.fixture(autouse=True)
    def _case_resources(self) -> Iterator[None]:
        """Create one complete ready session in an isolated runtime directory."""
        with ExitStack() as cleanup:
            self.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(self.temporary)
            self.root = Path(self.temporary.name) / "runtime"
            self.root.mkdir(mode=0o700)
            yield

    def test_openssh_interprets_generated_private_session_config(self) -> None:
        """OpenSSH receives the bound endpoint and disables ambient forwarding state."""
        session = create_ready_session(self.root)
        with mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root):
            ssh_transport._mark_current(session)  # noqa: SLF001

            path = self.root / "current" / "phone.ssh-config"
            metadata = path.lstat()
            assert path.is_file()
            assert not (path.is_symlink())
            assert (metadata.st_mode & 0o777) == (0o600)

            ssh = shutil.which("ssh")
            assert (ssh) is not None
            parsed = run_process(
                [str(ssh), "-G", "-F", str(path), "fplinux"],
                name="OpenSSH generated configuration parse",
                timeout=_SSH_CONFIG_TIMEOUT_SECONDS,
            )
            assert (parsed.returncode) == (0), parsed.stderr
            effective = dict(line.split(maxsplit=1) for line in parsed.stdout.splitlines())
            assert (effective["hostname"]) == (session["phone_address"])
            assert (effective["user"]) == ("root")
            assert (effective["identityfile"]) == (session["private_key"])
            assert (effective["userknownhostsfile"]) == (session["known_hosts"])
            assert (effective["hostkeyalias"]) == (f"fplinux-{session['usb_serial']}")
            assert (effective["bindaddress"]) == (session["host_address"])
            assert (effective["controlmaster"]) == ("auto")
            assert (effective["controlpersist"]) == ("60")
            assert (effective["controlpath"]) == (str(Path(session["private_key"]).parent / "mux"))
            assert (effective["serveraliveinterval"]) == ("5")
            assert (effective["serveralivecountmax"]) == ("3")
            assert (effective["passwordauthentication"]) == ("no")
            assert (effective["identityagent"]) == ("none")
            assert (effective["forwardagent"]) == ("no")
            assert ("proxycommand") not in (effective)

            ssh_transport._cleanup_target_sessions(self.root, "phone")  # noqa: SLF001

        assert not (path.exists())
        assert not ((self.root / "current" / "phone.json").exists())


class OpenSshServerLifecycleTests:
    """Observe ownership of the real daemon across a controlled readiness failure."""

    def test_readiness_failure_stops_and_reaps_launched_daemon(
        self, request: pytest.FixtureRequest
    ) -> None:
        """An exception after launching sshd leaves neither a live child nor a zombie."""
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            server = OpenSshServer(base / "server", base / "client_ed25519")
            request.addfinalizer(server.stop)
            daemon: subprocess.Popen[bytes] | None = None
            wait_until_listening = server._wait_until_listening  # noqa: SLF001

            def fail_readiness() -> None:
                nonlocal daemon
                wait_until_listening()
                daemon = server.process
                if daemon is None:
                    pytest.fail("OpenSSH fixture did not launch an owned daemon")
                assert (daemon.poll()) is None
                message = "controlled OpenSSH readiness failure"
                raise RuntimeError(message)

            with (
                mock.patch.object(server, "_wait_until_listening", side_effect=fail_readiness),
                pytest.raises(RuntimeError, match="controlled OpenSSH readiness failure"),
            ):
                server.start()

            if daemon is None:
                pytest.fail("readiness failure did not observe the launched daemon")
            assert (daemon.returncode) is not None
            assert (process_state(daemon.pid)) is None
            assert (server.process) is None


class SshTransportOpenSshReuseTests:
    """Exercise session reuse against a real loopback OpenSSH server."""

    @pytest.fixture(autouse=True)
    def _case_resources(self) -> Iterator[None]:
        """Create test-owned private runtime and server directories."""
        with ExitStack() as cleanup:
            self.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(self.temporary)
            self.base = Path(self.temporary.name)
            yield

    @staticmethod
    def _ssh_command(config: Path, server: OpenSshServer, command: str) -> list[str]:
        ssh = shutil.which("ssh")
        if ssh is None:
            message = "host-tool SSH test requires ssh"
            raise RuntimeError(message)
        return [
            ssh,
            "-F",
            str(config),
            "-l",
            server.user,
            "-p",
            str(server.port),
            "-o",
            "HostName=127.0.0.1",
            "fplinux",
            command,
        ]

    def _run_ssh(
        self,
        config: Path,
        server: OpenSshServer,
        command: str,
    ) -> subprocess.CompletedProcess[str]:
        return run_process(
            self._ssh_command(config, server, command),
            name="loopback OpenSSH command",
            timeout=_SSH_CONFIG_TIMEOUT_SECONDS,
        )

    @staticmethod
    def _run_sftp(
        config: Path,
        server: OpenSshServer,
        command: str,
    ) -> subprocess.CompletedProcess[str]:
        sftp = shutil.which("sftp")
        if sftp is None:
            message = "host-tool SSH test requires sftp"
            raise RuntimeError(message)
        return subprocess.run(
            [
                sftp,
                "-q",
                "-F",
                str(config),
                "-P",
                str(server.port),
                "-o",
                f"User={server.user}",
                "-o",
                "HostName=127.0.0.1",
                "-b",
                "-",
                "fplinux",
            ],
            input=command + "\n",
            capture_output=True,
            text=True,
            timeout=_SSH_CONFIG_TIMEOUT_SECONDS,
            check=False,
        )

    def _publish_loopback_session(
        self,
        root: Path,
        server: OpenSshServer,
        *,
        persist_seconds: str,
        session: dict[str, Any] | None = None,
        target: str = "phone",
    ) -> tuple[dict[str, Any], Path]:
        if session is None:
            session = create_ready_session(root, target=target)
        if Path(session["private_key"]) != server.client_key:
            shutil.copyfile(server.client_key, session["private_key"])
            Path(session["private_key"]).chmod(0o600)
        alias = f"fplinux-{session['usb_serial']}"
        Path(session["known_hosts"]).write_text(
            server.host_key_line(alias),
            encoding="ascii",
        )
        original_options = ssh_transport._ssh_option_values  # noqa: SLF001

        def loopback_options(
            value: dict[str, Any],
            *,
            connect_timeout: int = 5,
        ) -> list[tuple[str, str]]:
            options = original_options(value, connect_timeout=connect_timeout)
            replacements = {
                "BindAddress": "127.0.0.1",
                "ControlPersist": persist_seconds,
            }
            return [(name, replacements.get(name, option)) for name, option in options]

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=root),
            mock.patch.object(ssh_transport, "SSH_PORT", server.port),
            mock.patch.object(ssh_transport, "_ssh_option_values", side_effect=loopback_options),
        ):
            ssh_transport._mark_current(session)  # noqa: SLF001
        return session, root / "current" / f"{target}.ssh-config"

    @staticmethod
    def _loopback_ssh_argv(server: OpenSshServer) -> Callable[..., list[str]]:
        original_ssh_argv = ssh_transport._ssh_argv  # noqa: SLF001

        def loopback_ssh_argv(
            value: dict[str, Any],
            command: str | None = None,
            *,
            connect_timeout: int = 5,
            shared: bool = True,
        ) -> list[str]:
            argv = original_ssh_argv(
                value,
                command,
                connect_timeout=connect_timeout,
                shared=shared,
            )
            bind_option = f"BindAddress={value['host_address']}"
            argv = [
                "BindAddress=127.0.0.1" if argument == bind_option else argument
                for argument in argv
            ]
            destination = f"root@{value['phone_address']}"
            argv[argv.index(destination)] = f"{server.user}@127.0.0.1"
            argv[1:1] = ["-p", str(server.port)]
            return argv

        return loopback_ssh_argv

    @staticmethod
    def _cleanup_session(root: Path, target: str = "phone") -> None:
        with mock.patch.object(ssh_transport, "_runtime_root", return_value=root):
            ssh_transport._cleanup_target_sessions(root, target)  # noqa: SLF001

    @staticmethod
    def _wait_until_absent(path: Path) -> None:
        deadline = time.monotonic() + _IDLE_EXPIRY_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            if not path.exists():
                return
            time.sleep(0.02)
        raise AssertionError(f"OpenSSH control socket did not expire: {path}")

    @staticmethod
    def _wait_until_process_absent(process_id: int) -> None:
        deadline = time.monotonic() + _IDLE_EXPIRY_TIMEOUT_SECONDS
        process_path = Path(f"/proc/{process_id}")
        while time.monotonic() < deadline:
            if not process_path.exists():
                return
            time.sleep(0.02)
        raise AssertionError(f"timed-out remote stream process is still running: {process_id}")

    def test_generated_config_exec_and_sftp_share_connection_until_idle_expiry(
        self, request: pytest.FixtureRequest
    ) -> None:
        """Generated-config exec and SFTP share and expire one master."""
        reference_runtime = "/run/user/1000/fplinux"
        padding = len(reference_runtime) - len(str(self.base)) - 1
        assert (padding) > (0)
        root = self.base / ("r" * padding)
        assert (len(str(root))) == (len(reference_runtime))
        root.mkdir(mode=0o700)
        target = "inoi-244-modern-4g"
        session = create_ready_session(root, target=target)
        server = OpenSshServer(self.base / "server", Path(session["private_key"]))
        server.start()
        request.addfinalizer(server.stop)
        request.addfinalizer(partial(self._cleanup_session, root, target))
        session, config = self._publish_loopback_session(
            root,
            server,
            persist_seconds="1",
            session=session,
            target=target,
        )
        control_path = Path(session["private_key"]).parent / "mux"

        first = self._run_ssh(config, server, 'printf "%s\\n" "$SSH_CONNECTION"')
        assert (first.returncode) == (0), first.stderr
        connection = first.stdout.strip()
        assert (len(connection.split())) == (4)
        server.wait_for_connections(1)
        assert control_path.is_socket()

        failed = self._run_ssh(config, server, "printf 'expected failure\\n'; exit 37")
        assert (failed.returncode) == (37)
        assert (failed.stdout) == ("expected failure\n")

        source = self.base / "upload-source"
        destination = self.base / "uploaded-by-sftp"
        source.write_bytes(b"one authenticated SFTP payload\n")
        transferred = self._run_sftp(
            config,
            server,
            f'put "{source}" "{destination}"',
        )
        assert (transferred.returncode) == (0), transferred.stderr
        assert (destination.read_bytes()) == (source.read_bytes())

        reused = self._run_ssh(config, server, 'printf "%s\\n" "$SSH_CONNECTION"')
        assert (reused.returncode) == (0), reused.stderr
        assert (reused.stdout.strip()) == (connection)
        assert (server.accepted_connections()) == (1)

        self._wait_until_absent(control_path)
        after_expiry = self._run_ssh(config, server, 'printf "%s\\n" "$SSH_CONNECTION"')
        assert (after_expiry.returncode) == (0), after_expiry.stderr
        assert (after_expiry.stdout.strip()) != (connection)
        server.wait_for_connections(2)

    def test_run_remote_isolates_arbitrary_commands_and_shares_internal_queries(
        self, request: pytest.FixtureRequest
    ) -> None:
        """Arbitrary commands use dedicated TCP while internal queries share one master."""
        root = self.base / "runtime"
        root.mkdir(mode=0o700)
        session = create_ready_session(root)
        server = OpenSshServer(self.base / "server", Path(session["private_key"]))
        server.start()
        request.addfinalizer(server.stop)
        request.addfinalizer(partial(self._cleanup_session, root))
        session, _config = self._publish_loopback_session(
            root,
            server,
            persist_seconds="60",
            session=session,
        )
        loopback_ssh_argv = self._loopback_ssh_argv(server)

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=root),
            mock.patch.object(
                ssh_transport,
                "_ssh_argv",
                side_effect=loopback_ssh_argv,
            ),
        ):
            first = ssh_transport.run_remote(
                session,
                'printf "%s\\n" "$SSH_CONNECTION"',
                capture_output=True,
            )
            failed = ssh_transport.run_remote(
                session,
                'printf "%s\\n" "$SSH_CONNECTION"; printf "expected stderr\\n" >&2; exit 23',
                capture_output=True,
            )
            shared_first = ssh_transport.run_remote(
                session,
                'printf "%s\\n" "$SSH_CONNECTION"',
                capture_output=True,
                shared=True,
            )
            shared_second = ssh_transport.run_remote(
                session,
                'printf "%s\\n" "$SSH_CONNECTION"',
                capture_output=True,
                shared=True,
            )

        assert (first.returncode) == (0), first.stderr
        assert (failed.returncode) == (23)
        assert (failed.stderr) == ("expected stderr\n")
        assert (first.stdout) != (failed.stdout)
        assert (shared_first.returncode) == (0), shared_first.stderr
        assert (shared_second.returncode) == (0), shared_second.stderr
        assert (shared_first.stdout) == (shared_second.stdout)
        assert (shared_first.stdout) not in ({first.stdout, failed.stdout})
        server.wait_for_connections(3)
        server.wait_for_disconnections(2)

    def test_separate_sessions_keep_independent_masters_and_cleanup(
        self, request: pytest.FixtureRequest
    ) -> None:
        """Cleaning one session stops its master without disturbing another session."""
        first_root = self.base / "runtime-first"
        second_root = self.base / "runtime-second"
        first_root.mkdir(mode=0o700)
        second_root.mkdir(mode=0o700)
        initial = create_ready_session(first_root)
        server = OpenSshServer(self.base / "server", Path(initial["private_key"]))
        server.start()
        request.addfinalizer(server.stop)
        request.addfinalizer(partial(self._cleanup_session, second_root))
        request.addfinalizer(partial(self._cleanup_session, first_root))
        first, first_config = self._publish_loopback_session(
            first_root,
            server,
            persist_seconds="60",
            session=initial,
        )
        second, second_config = self._publish_loopback_session(
            second_root,
            server,
            persist_seconds="60",
        )

        first_result = self._run_ssh(
            first_config,
            server,
            'printf "%s\\n" "$SSH_CONNECTION"',
        )
        second_result = self._run_ssh(
            second_config,
            server,
            'printf "%s\\n" "$SSH_CONNECTION"',
        )
        assert (first_result.returncode) == (0), first_result.stderr
        assert (second_result.returncode) == (0), second_result.stderr
        assert (first_result.stdout) != (second_result.stdout)
        server.wait_for_connections(2)
        first_socket = Path(first["private_key"]).parent / "mux"
        second_socket = Path(second["private_key"]).parent / "mux"
        assert first_socket.is_socket()
        assert second_socket.is_socket()

        self._cleanup_session(first_root)
        server.wait_for_disconnections(1)
        assert not (first_socket.exists())
        assert second_socket.is_socket()
        reused = self._run_ssh(
            second_config,
            server,
            'printf "%s\\n" "$SSH_CONNECTION"',
        )
        assert (reused.returncode) == (0), reused.stderr
        assert (reused.stdout) == (second_result.stdout)
        assert (server.accepted_connections()) == (2)

    def test_stream_timeout_stops_remote_producer_without_closing_adjacent_channel(
        self, request: pytest.FixtureRequest
    ) -> None:
        """Timing out one producer preserves a concurrent channel on the same master."""
        root = self.base / "runtime"
        root.mkdir(mode=0o700)
        session = create_ready_session(root)
        server = OpenSshServer(self.base / "server", Path(session["private_key"]))
        server.start()
        request.addfinalizer(server.stop)
        request.addfinalizer(partial(self._cleanup_session, root))
        session, config = self._publish_loopback_session(
            root,
            server,
            persist_seconds="60",
            session=session,
        )
        established = self._run_ssh(config, server, 'printf "%s\\n" "$SSH_CONNECTION"')
        assert (established.returncode) == (0), established.stderr
        server.wait_for_connections(1)
        loopback_ssh_argv = self._loopback_ssh_argv(server)
        producer_pid = self.base / "stream-producer.pid"

        adjacent = subprocess.Popen(
            self._ssh_command(
                config,
                server,
                'sleep 0.6; printf "neighbor\\n%s\\n" "$SSH_CONNECTION"',
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            with (
                tempfile.TemporaryFile(mode="w+b") as destination,
                mock.patch.object(ssh_transport, "_runtime_root", return_value=root),
                mock.patch.object(
                    ssh_transport,
                    "_ssh_argv",
                    side_effect=loopback_ssh_argv,
                ),
            ):
                with pytest.raises(SystemExit, match=r"timed out after 0.3s"):
                    ssh_transport.stream_remote(
                        session,
                        f"printf '%s\\n' \"$$\" > '{producer_pid}'; "
                        "while :; do printf 'stream-data\\n'; sleep 0.01; done",
                        destination,
                        timeout=0.3,
                    )
                destination.seek(0)
                assert destination.read().startswith(b"stream-data\n")
            process_id = int(producer_pid.read_text(encoding="ascii").strip())
            self._wait_until_process_absent(process_id)
            adjacent_stdout, adjacent_stderr = adjacent.communicate(timeout=3)
        finally:
            if adjacent.poll() is None:
                adjacent.kill()
                adjacent.communicate()
        assert (adjacent.returncode) == (0), adjacent_stderr
        assert (adjacent_stdout.splitlines()[0]) == ("neighbor")
        assert (adjacent_stdout.splitlines()[1]) == (established.stdout.strip())
        server.wait_for_connections(2)
        server.wait_for_disconnections(1)

    def test_persistent_master_releases_inherited_flock_and_captured_stdout(
        self, request: pytest.FixtureRequest
    ) -> None:
        """A background master does not retain its launcher's lock or output pipe."""
        root = self.base / "runtime"
        root.mkdir(mode=0o700)
        session = create_ready_session(root)
        server = OpenSshServer(self.base / "server", Path(session["private_key"]))
        server.start()
        request.addfinalizer(server.stop)
        request.addfinalizer(partial(self._cleanup_session, root))
        session, config = self._publish_loopback_session(
            root,
            server,
            persist_seconds="60",
            session=session,
        )
        lock_path = self.base / "session.lock"

        with lock_path.open("w", encoding="ascii") as held_lock:
            fcntl.flock(held_lock, fcntl.LOCK_EX)
            launched = subprocess.run(
                self._ssh_command(config, server, "printf 'complete\\n'"),
                pass_fds=(held_lock.fileno(),),
                capture_output=True,
                text=True,
                timeout=_SSH_CONFIG_TIMEOUT_SECONDS,
                check=False,
            )
        assert (launched.returncode) == (0), launched.stderr
        assert (launched.stdout) == ("complete\n")
        control_path = Path(session["private_key"]).parent / "mux"
        assert control_path.is_socket()
        server.wait_for_connections(1)

        with lock_path.open("w", encoding="ascii") as probe:
            try:
                fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                pytest.fail(f"persistent SSH master retained inherited flock: {error}")
