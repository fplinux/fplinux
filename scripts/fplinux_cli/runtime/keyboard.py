# SPDX-License-Identifier: GPL-2.0-only
"""Verify the session owner and forward an explicitly selected host keyboard."""

from __future__ import annotations

import os
import pwd
import subprocess
import sys
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.artifacts.host_tools import read_cached_host_tool
from fplinux_cli.common import fail
from fplinux_cli.data_paths import data_root
from fplinux_cli.manifests.values import TARGET_NAME

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.artifacts.bundles import CurrentBundle


def _keyboard_client(bundle: CurrentBundle | None) -> Path:
    if bundle is None:
        slot = data_root(common.ROOT) / "host-tools/fplinux-usb-keyboard"
        client = read_cached_host_tool(slot)
        if client is None:
            fail("no prepared USB keyboard client; build the target before forwarding a keyboard")
        return client
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


def _invoking_account() -> pwd.struct_passwd | None:
    """Resolve sudo's invoking user before reading that user's session credentials."""
    sudo_uid = os.environ.get("SUDO_UID")
    if os.geteuid() != 0 or sudo_uid is None or sudo_uid == "0":
        return None
    if not sudo_uid.isascii() or not sudo_uid.isdecimal():
        fail("sudo keyboard verification requires a valid SUDO_UID")
    uid = int(sudo_uid)
    try:
        account = pwd.getpwuid(uid)
    except KeyError, OverflowError:
        fail("sudo keyboard verification cannot resolve the invoking user")
    if account.pw_name != os.environ.get("SUDO_USER"):
        fail("sudo keyboard verification requires the matching invoking user")
    return account


def _run_as_owner(
    command: list[str], *, capture: bool = False
) -> subprocess.CompletedProcess[str]:
    """Run checkout selection or verification with the session owner's credentials."""
    account = _invoking_account()
    if account is None:
        return subprocess.run(
            command, check=False, stdout=subprocess.PIPE if capture else None, text=True
        )
    environment = {
        **os.environ,
        "HOME": account.pw_dir,
        "USER": account.pw_name,
        "LOGNAME": account.pw_name,
    }
    try:
        result = subprocess.run(
            command,
            env=environment,
            user=account.pw_uid,
            group=account.pw_gid,
            extra_groups=(),
            check=False,
            stdout=subprocess.PIPE if capture else None,
            text=True,
        )
    except OSError as error:
        fail(f"cannot verify the keyboard session as {account.pw_name}: {error}")
    return result


def resolve_keyboard_target(target: str | None) -> str:
    """Select a live target as the session owner, without reading credentials as root."""
    if target is not None:
        return target
    account = _invoking_account()
    if account is None:
        from fplinux_cli.cli.runtime import _checkout_connection  # noqa: PLC0415

        return str(_checkout_connection(None)[1]["target"])
    program = (
        "import sys; sys.path.insert(0, sys.argv[1] + '/scripts'); "
        "from fplinux_cli.cli.runtime import _checkout_connection; "
        "print(_checkout_connection(None)[1]['target'])"
    )
    result = _run_as_owner([sys.executable, "-I", "-c", program, str(common.ROOT)], capture=True)
    if result.returncode:
        raise SystemExit(result.returncode)
    selected = result.stdout.strip()
    if TARGET_NAME.fullmatch(selected) is None:
        fail("could not select a valid keyboard target")
    return selected


def _verify_keyboard_session(
    target: str,
    *,
    profile: str | None,
    build_type: str,
    allow_other_build: bool,
    phone_data: Path | None = None,
) -> None:
    """Use the current checkout's console policy as the session owner."""
    command = [str(common.ROOT / "fplinux"), "console", target]
    if allow_other_build:
        command.append("--allow-other-build")
    else:
        command.extend(["--build-type", build_type])
        if profile is not None:
            command.extend(["--profile", profile])
        if phone_data is not None:
            command.extend(["--phone-data", str(phone_data.resolve())])
    command.extend(["--exec", "true"])
    result = _run_as_owner(command)
    if result.returncode:
        raise SystemExit(result.returncode)


def forward_keyboard(  # noqa: PLR0913 -- the input device, bundle and phone selection are distinct.
    bundle: CurrentBundle | None,
    target: str,
    config: dict[str, Any],
    keyboard: str,
    *,
    profile: str | None,
    build_type: str = "release",
    allow_other_build: bool = False,
    phone_data: Path | None = None,
) -> None:
    """Replace this process with the keyboard bridge after authenticating its session."""
    _verify_keyboard_session(
        target,
        profile=profile,
        build_type=build_type,
        allow_other_build=allow_other_build,
        phone_data=phone_data,
    )
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
