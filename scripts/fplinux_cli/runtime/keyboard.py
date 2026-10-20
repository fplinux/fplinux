# SPDX-License-Identifier: GPL-2.0-only
"""Verify the session owner and forward an explicitly selected host keyboard."""

from __future__ import annotations

import os
import pwd
import subprocess
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import fail
from fplinux_cli.runtime.bundle_session import bundle_runtime_command
from fplinux_cli.runtime.runner import reconnect_bundle

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.artifacts.bundles import CurrentBundle


def _keyboard_client(bundle: CurrentBundle) -> Path:
    client = bundle.path / "host/fplinux-usb-keyboard"
    if client.is_symlink() or not client.is_file():
        fail(f"current bundle has no valid USB keyboard client: {client}")
    return client


def _keyboard_connection(config: dict[str, Any]) -> list[str]:
    gadget = config["runtime"]["usb"]["linux_gadget"]
    return [
        "--vid",
        f"{gadget['vendor_id']:04x}",
        "--pid",
        f"{gadget['product_id']:04x}",
        "--wait",
        str(gadget["wait_seconds"]),
    ]


def _keyboard_interface(config: dict[str, Any]) -> str:
    """Return the runtime-declared generic-serial keyboard interface."""
    return str(config["runtime"]["usb"]["linux_gadget"]["keyboard_interface"])


def _verify_keyboard_session(
    bundle: CurrentBundle,
    target: str,
    *,
    profile: str | None,
) -> None:
    """Verify as the session owner while keeping an explicitly sudoed keyboard privileged."""
    sudo_uid = os.environ.get("SUDO_UID")
    if os.geteuid() != 0 or sudo_uid is None or sudo_uid == "0":
        reconnect_bundle(bundle, target, profile=profile, exec_command="true")
        return
    if not sudo_uid.isascii() or not sudo_uid.isdecimal():
        fail("sudo keyboard verification requires a valid SUDO_UID")
    uid = int(sudo_uid)
    try:
        account = pwd.getpwuid(uid)
    except KeyError, OverflowError:
        fail("sudo keyboard verification cannot resolve the invoking user")
    if account.pw_name != os.environ.get("SUDO_USER"):
        fail("sudo keyboard verification requires the matching invoking user")
    environment = {
        **os.environ,
        "HOME": account.pw_dir,
        "USER": account.pw_name,
        "LOGNAME": account.pw_name,
    }
    command = bundle_runtime_command(bundle, "run", target=target, profile=profile)
    command.extend(["--reconnect", "--exec", "true"])
    try:
        result = subprocess.run(
            command,
            env=environment,
            user=uid,
            group=account.pw_gid,
            extra_groups=(),
            check=False,
        )
    except OSError as error:
        fail(f"cannot verify the keyboard session as {account.pw_name}: {error}")
    if result.returncode:
        raise SystemExit(result.returncode)


def forward_keyboard(
    bundle: CurrentBundle,
    target: str,
    config: dict[str, Any],
    keyboard: str,
    *,
    profile: str | None,
) -> None:
    """Replace this process with the keyboard bridge after authenticating its session."""
    _verify_keyboard_session(bundle, target, profile=profile)
    client = _keyboard_client(bundle)
    arguments = [
        str(client),
        *_keyboard_connection(config),
        "--interface",
        _keyboard_interface(config),
        "--keyboard",
        keyboard,
    ]
    os.execv(client, arguments)
