# SPDX-License-Identifier: GPL-2.0-only
"""Verify the session owner and forward an explicitly selected host keyboard."""

from __future__ import annotations

import os
import pwd
import stat
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.runtime.bundle_session import _current_ssh_session

if TYPE_CHECKING:
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


def _sudo_keyboard_runtime_directory(uid: int) -> Path:
    """Find the invoking user's existing runtime directory without borrowing root state."""
    inherited = os.environ.get("XDG_RUNTIME_DIR")
    candidates = ([Path(inherited)] if inherited else []) + [Path(f"/run/user/{uid}")]
    for candidate in candidates:
        if not candidate.is_absolute():
            continue
        try:
            metadata = candidate.lstat()
        except OSError:
            continue
        if stat.S_ISDIR(metadata.st_mode) and metadata.st_uid == uid:
            return candidate
    return fail(f"keyboard verification cannot find an existing runtime directory for UID {uid}")


def _verify_keyboard_session(
    bundle: CurrentBundle,
    manifest: dict[str, Any],
    target: str,
    *,
    profile: str | None,
    build_type: str,
) -> None:
    """Verify as the session owner while keeping an explicitly sudoed keyboard privileged."""
    sudo_uid = os.environ.get("SUDO_UID")
    if os.geteuid() != 0 or sudo_uid is None or sudo_uid == "0":
        _current_ssh_session(bundle, manifest, target)
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
    runtime = _sudo_keyboard_runtime_directory(uid)
    environment = {
        **os.environ,
        "HOME": account.pw_dir,
        "USER": account.pw_name,
        "LOGNAME": account.pw_name,
        "XDG_RUNTIME_DIR": str(runtime),
    }
    command = [
        str(common.ROOT / "fplinux"),
        "console",
        target,
        "--build-type",
        build_type,
        "--exec",
        "true",
    ]
    if profile is not None:
        command.extend(["--profile", profile])
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


def forward_keyboard(  # noqa: PLR0913 -- bundle, controls and privilege policies stay explicit.
    bundle: CurrentBundle,
    manifest: dict[str, Any],
    target: str,
    config: dict[str, Any],
    keyboard: str,
    *,
    profile: str | None,
    build_type: str,
) -> None:
    """Replace this process with the keyboard bridge after authenticating its session."""
    _verify_keyboard_session(bundle, manifest, target, profile=profile, build_type=build_type)
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
