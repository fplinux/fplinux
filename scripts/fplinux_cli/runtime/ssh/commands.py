# SPDX-License-Identifier: GPL-2.0-only
"""OpenSSH command and configuration policy."""

from __future__ import annotations

import os
import shutil
from typing import TYPE_CHECKING, Any

from .errors import fail

if TYPE_CHECKING:
    from typing import BinaryIO

SSH_PORT = 22


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        fail(f"required host tool is missing: {name}")
    return path


def ssh_policy_options(
    *,
    usb_serial: str,
    known_hosts: str,
    host_address: str,
    connect_timeout: int = 5,
) -> list[tuple[str, str]]:
    """Bind public-key authentication to one phone without ambient client routing."""
    return [
        ("BatchMode", "yes"),
        ("ServerAliveInterval", "5"),
        ("ServerAliveCountMax", "3"),
        ("IdentitiesOnly", "yes"),
        ("IdentityAgent", "none"),
        ("PasswordAuthentication", "no"),
        ("KbdInteractiveAuthentication", "no"),
        ("PreferredAuthentications", "publickey"),
        ("NumberOfPasswordPrompts", "0"),
        ("StrictHostKeyChecking", "yes"),
        ("UserKnownHostsFile", known_hosts),
        ("HostKeyAlias", f"fplinux-{usb_serial}"),
        ("CheckHostIP", "no"),
        ("ClearAllForwardings", "yes"),
        ("ForwardAgent", "no"),
        ("ForwardX11", "no"),
        ("Tunnel", "no"),
        ("PermitLocalCommand", "no"),
        ("ProxyCommand", "none"),
        ("ProxyJump", "none"),
        ("BindAddress", host_address),
        ("ConnectTimeout", str(connect_timeout)),
        ("ConnectionAttempts", "1"),
        ("LogLevel", "ERROR"),
    ]


def _ssh_option_values(
    session: dict[str, Any],
    *,
    connect_timeout: int = 5,
) -> list[tuple[str, str]]:
    """Combine the phone policy with Unix identity and direct connection ownership."""
    return [
        ("ControlMaster", "no"),
        ("ControlPersist", "no"),
        ("ControlPath", "none"),
        ("IdentityFile", str(session["private_key"])),
        ("GlobalKnownHostsFile", "/dev/null"),
        *ssh_policy_options(
            usb_serial=session["usb_serial"],
            known_hosts=str(session["known_hosts"]),
            host_address=str(session["host_address"]),
            connect_timeout=connect_timeout,
        ),
    ]


def ssh_options(session: dict[str, Any], *, connect_timeout: int = 5) -> list[str]:
    """Quote session paths for OpenSSH's own option parser."""
    arguments = ["-F", "/dev/null"]
    for name, value in _ssh_option_values(session, connect_timeout=connect_timeout):
        rendered = value
        if name in {"IdentityFile", "UserKnownHostsFile"}:
            rendered = ssh_config_value(value)
        arguments.extend(("-o", f"{name}={rendered}"))
    return arguments


def ssh_config_value(value: str) -> str:
    """Quote an OpenSSH configuration argument without allowing a new directive."""
    if any(character in value for character in ("\0", "\n", "\r")):
        fail("SSH configuration value is not a single line")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _ssh_config_directive(name: str, value: str) -> str:
    """Render one directive, preserving OpenSSH's hard-coded ``none`` sentinels."""
    literal_none = {"ControlPath", "IdentityAgent", "ProxyCommand", "ProxyJump"}
    rendered = "none" if name in literal_none and value == "none" else ssh_config_value(value)
    return f"    {name} {rendered}\n"


def ssh_config(session: dict[str, Any]) -> str:
    """Create a direct, session-scoped OpenSSH configuration for ``ssh -F``."""
    directives = [
        ("HostName", str(session["phone_address"])),
        ("User", "root"),
        ("Port", str(SSH_PORT)),
        *_ssh_option_values(session),
    ]
    return "".join(
        (
            "Host fplinux\n",
            *(_ssh_config_directive(name, value) for name, value in directives),
        )
    )


def ssh_argv(
    session: dict[str, Any],
    command: str | None = None,
    *,
    connect_timeout: int = 5,
) -> list[str]:
    argv = [
        require_tool("ssh"),
        *ssh_options(session, connect_timeout=connect_timeout),
        f"root@{session['phone_address']}",
    ]
    if command is not None:
        argv.append(command)
    return argv


def stream_stderr_detail(stream: BinaryIO) -> str:
    """Return a bounded diagnostic without ever mixing it into downloaded bytes."""
    stream.seek(0, os.SEEK_END)
    stream.seek(max(0, stream.tell() - 4096))
    return stream.read().decode("utf-8", errors="replace").strip()
