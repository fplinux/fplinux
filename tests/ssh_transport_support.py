# SPDX-License-Identifier: GPL-2.0-only
"""Valid private session inputs shared by SSH transport test tiers."""

from __future__ import annotations

import hashlib
import json
from typing import TYPE_CHECKING, Any

from fplinux_cli import ssh_transport

if TYPE_CHECKING:
    from pathlib import Path

TEST_BUNDLE_IDENTITY = {"bundle_generation": "1" * 64}


def create_ready_session(
    root: Path,
    *,
    status: str = "ready",
    identity: dict[str, str] | None = None,
) -> dict[str, Any]:
    """Create one complete private session directory for a transport consumer."""
    session_id = b"session identity is exactly 32!!"
    usb_serial = hashlib.sha256(session_id).hexdigest()[:32]
    directory = root / "sessions" / f"phone.{usb_serial}"
    directory.mkdir(parents=True, mode=0o700)
    owner = {
        "kind": ssh_transport.SESSION_OWNER_KIND,
        "target": "phone",
        "usb_serial": usb_serial,
    }
    owner_path = directory / "owner.json"
    owner_path.write_text(json.dumps(owner), encoding="utf-8")
    owner_path.chmod(0o600)
    private_key = directory / "client_ed25519"
    known_hosts = directory / "known_hosts"
    for path in (private_key, known_hosts):
        path.write_text("private\n", encoding="ascii")
        path.chmod(0o600)
    state: dict[str, Any] = {
        "target": "phone",
        "session_id": session_id.hex(),
        "usb_serial": usb_serial,
        "network": "10.23.45.0/30",
        "host_address": "10.23.45.1",
        "phone_address": "10.23.45.2",
        "host_mac": "02:00:00:00:00:01",
        "device_mac": "02:00:00:00:00:02",
        "private_key": str(private_key),
        "known_hosts": str(known_hosts),
        "image": str(directory / "ramboot.bin"),
        "vendor_id": 0x0525,
        "product_id": 0xA4A6,
        "wait_seconds": 1,
        "status": status,
        "interface": "usb0",
        **(TEST_BUNDLE_IDENTITY if identity is None else identity),
    }
    state_path = directory / "session.json"
    state_path.write_text(json.dumps(state), encoding="utf-8")
    state_path.chmod(0o600)
    return state
