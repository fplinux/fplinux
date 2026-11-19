# SPDX-License-Identifier: GPL-2.0-only
"""Use an enrolled card's profile with real OpenSSH clients and keys."""

from __future__ import annotations

import os
import shlex
import shutil
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING

from fplinux_cli.runtime import card_access
from fplinux_cli.runtime.windows.transport import WindowsTransport

from tests.host_tool.runtime.openssh_server import OpenSshServer
from tests.process import run_process
from tests.windows_host_support import StubWindowsHost

if TYPE_CHECKING:
    import pytest


def test_saved_card_config_accepts_only_the_pinned_server(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    user_data = tmp_path / "user data"
    monkeypatch.setenv("XDG_DATA_HOME", str(user_data))
    monkeypatch.setattr(card_access, "host_route_networks", set)
    session = {"network": "10.23.45.0/30"}
    server = OpenSshServer(tmp_path / "server", tmp_path / "unrelated_key")
    with card_access.prepare_card_access(
        (None, session), "demo-phone", "a" * 32, tmp_path
    ) as access:
        server.start()
        try:
            client_body = (tmp_path / "client-key").read_text(encoding="ascii")
            server.authorized_keys.write_text(f"ssh-ed25519 {client_body}\n", encoding="ascii")
            host_body = server.host_key_line("card").split()[2]
            access.pin_host_key(host_body)
            host, config_flag, config, alias = shlex.split(access.command)
            args = [
                host,
                config_flag,
                config,
                "-l",
                server.user,
                "-p",
                str(server.port),
                "-o",
                "HostName=127.0.0.1",
                "-o",
                "BindAddress=127.0.0.1",
                alias,
                "printf 'connected to the card\\n'",
            ]
            trusted = run_process(args, name="connect using saved card profile", timeout=10)
            assert trusted.returncode == 0, trusted.stderr
            assert trusted.stdout == "connected to the card\n"

            access.pin_host_key(client_body)
            untrusted = run_process(args, name="reject mismatched card host key", timeout=10)
            assert untrusted.returncode == 255, untrusted.stderr
            assert "Host key verification failed" in untrusted.stderr
        finally:
            server.stop()


def test_windows_card_uses_the_archive_client_and_rndis(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    keygen = shutil.which("ssh-keygen")
    assert keygen is not None
    ssh = tmp_path / "Alice's archive" / "tools/openssh/ssh.exe"
    user_data = tmp_path / "Local App Data/FPLinux"
    monkeypatch.setattr(card_access, "os", SimpleNamespace(name="nt", fsync=os.fsync))
    monkeypatch.setattr(card_access, "user_data_directory", lambda: user_data)
    transport = WindowsTransport(
        StubWindowsHost({"routes": [{"prefix": "10.0.0.0/8"}]}),
        {"ssh": ssh, "keygen": Path(keygen)},
        wait_seconds=10,
    )
    session = {"network": "10.23.45.0/30"}

    with card_access.prepare_card_access(
        (transport, session), "demo-phone", "b" * 32, tmp_path
    ) as access:
        assert b"transport=rndis\n" in (tmp_path / "usb-network").read_bytes()
        assert not str(access.network).startswith("10.")
        access.pin_host_key("AAAAC3NzaC1lZDI1NTE5AAAAIHJycnJycnJycnJycnJycnJycnJycnJycnJycnJycnJy")
        saved_config = access.directory / "ssh_config"
        assert saved_config.parent.parent.parent.parent == user_data / "cards"
        assert 'GlobalKnownHostsFile "NUL"' in saved_config.read_text(encoding="utf-8")
        assert access.command == (
            f"& '{tmp_path}/Alice''s archive/tools/openssh/ssh.exe' "
            f"'-F' '{saved_config}' 'fplinux'"
        )
