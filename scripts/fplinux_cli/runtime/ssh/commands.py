# SPDX-License-Identifier: GPL-2.0-only
"""OpenSSH command and configuration policy."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from typing import TYPE_CHECKING, Any

from .errors import fail

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import IO, BinaryIO

SSH_PORT = 22
_REMOTE_PATH = re.compile(r"/(?:[A-Za-z0-9._-]+/)*[A-Za-z0-9._-]+")


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
    global_known_hosts: str = "/dev/null",
) -> list[tuple[str, str]]:
    """Combine the phone policy with Unix identity and direct connection ownership."""
    return [
        ("ControlMaster", "no"),
        ("ControlPersist", "no"),
        ("ControlPath", "none"),
        ("IdentityFile", str(session["private_key"])),
        ("GlobalKnownHostsFile", global_known_hosts),
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
            if session.get("card_access"):
                rendered = rendered.replace("%", "%%")
            rendered = ssh_config_value(rendered)
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


def ssh_config(session: dict[str, Any], *, global_known_hosts: str = "/dev/null") -> str:
    """Create a direct, session-scoped OpenSSH configuration for ``ssh -F``."""
    directives = [
        ("HostName", str(session["phone_address"])),
        ("User", "root"),
        ("Port", str(SSH_PORT)),
        *_ssh_option_values(session, global_known_hosts=global_known_hosts),
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


def run_with_input(
    argv: list[str], write: Callable[[IO[bytes]], None], *, creationflags: int = 0
) -> subprocess.CompletedProcess[str]:
    """Send a generated binary stream while collecting command diagnostics separately."""
    with (
        tempfile.TemporaryFile() as stdout,
        tempfile.TemporaryFile() as stderr,
        subprocess.Popen(
            [argv[0], "-T", *argv[1:]],
            stdin=subprocess.PIPE,
            stdout=stdout,
            stderr=stderr,
            creationflags=creationflags,
        ) as process,
    ):
        try:
            if process.stdin is None:
                fail("SSH input pipe is unavailable")
            with process.stdin:
                write(process.stdin)
        except BrokenPipeError:
            # A reader that rejects the card can close the channel before taking input.
            pass
        except BaseException:
            process.kill()
            raise
        status = process.wait()
        stdout.seek(0)
        stderr.seek(0)
        return subprocess.CompletedProcess(
            argv,
            status,
            stdout.read().decode(errors="replace"),
            stderr.read().decode(errors="replace"),
        )


def remote_path(value: str, name: str) -> str:
    """Admit an absolute phone filename that is safe in the remote shell."""
    if len(value) > 200 or _REMOTE_PATH.fullmatch(value) is None:
        fail(f"{name} must be an absolute file path using only letters, digits, '.', '_', '-'")
    if any(component.strip(".") == "" for component in value.split("/")[1:]):
        fail(f"{name} must not contain dot-only path components")
    return value


def upload_space_check(directory: str, size: int) -> str:
    """Require space for the staged file and filesystem overhead."""
    free_kib = size // 1024 + 64
    return (
        f"[ -d '{directory}' ] && free=$(stat -f -c '%a %S' '{directory}') && "
        "set -- $free && "
        f'[ "$(($1 * $2 / 1024))" -ge {free_kib} ]'
    )


def upload_publication(temporary: str, destination: str, digest: str, size: int) -> str:
    """Verify a same-directory temporary file before replacing the destination."""
    return (
        f"got=$(sha256sum '{temporary}'); got=${{got%% *}}; "
        f"size=$(wc -c < '{temporary}'); "
        f'[ "$got" = \'{digest}\' ] && [ "$size" -eq {size} ] && '
        f"[ ! -d '{destination}' ] && [ ! -L '{destination}' ] && "
        f"mv -f '{temporary}' '{destination}'"
    )


def stream_stderr_detail(stream: BinaryIO) -> str:
    """Return a bounded diagnostic separate from binary transfer data."""
    stream.seek(0, os.SEEK_END)
    stream.seek(max(0, stream.tell() - 4096))
    return stream.read().decode("utf-8", errors="replace").strip()
