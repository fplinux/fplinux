# SPDX-License-Identifier: GPL-2.0-only
"""Windows private SSH session preparation."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import os
import secrets
import shutil
import subprocess
import tempfile
from contextlib import suppress
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.runtime.ssh import material

from .host import WindowsHost, console_creation_flags, fail

if TYPE_CHECKING:
    from collections.abc import Callable

    from .bundle import WindowsBundle


def write_session(host: WindowsHost, directory: Path, session: dict[str, Any]) -> None:
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=directory, prefix=".session.json.", mode="w", encoding="utf-8", delete=False
        ) as stream:
            temporary = Path(stream.name)
            json.dump(session, stream, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        host.secure_new(temporary)
        temporary.replace(directory / "session.json")
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def prepare_session(
    host: WindowsHost,
    bundle: WindowsBundle,
    *,
    events: Callable[[str], None] | None = None,
    session_root: Path,
    prepared_data_sha256: str | None = None,
) -> dict[str, Any]:
    root = host.runtime_root(session_root)
    if events is not None:
        events("network-preparation")
    occupied = {
        ipaddress.IPv4Network(route["prefix"], strict=False)
        for route in host.ps("routes.ps1")["routes"]
        if ipaddress.IPv4Network(route["prefix"], strict=False).prefixlen > 0
    }
    for path in root.glob("*/session.json"):
        previous = json.loads(path.read_text(encoding="utf-8"))
        occupied.add(ipaddress.IPv4Network(previous["network"]))
    for _attempt in range(4096):
        # The SSH helper owns the shared session addressing and binary ABI.
        network = material.candidate_network(secrets.token_bytes(5))
        if all(not network.overlaps(used) for used in occupied):
            break
    else:
        fail("Cannot allocate a free private USB network")
    session_id = secrets.token_bytes(32)
    serial = hashlib.sha256(session_id).hexdigest()[:32]
    directory = root / serial
    directory.mkdir()
    try:
        session = _prepare_session_directory(
            host, bundle, directory, network=network, session_id=session_id, events=events
        )
        session["prepared_data_sha256"] = prepared_data_sha256
        write_session(host, directory, session)
    except BaseException:
        with suppress(OSError):
            shutil.rmtree(directory)
        raise
    else:
        return session


def _prepare_session_directory(  # noqa: PLR0913 -- owned session inputs stay explicit.
    host: WindowsHost,
    bundle: WindowsBundle,
    directory: Path,
    *,
    network: ipaddress.IPv4Network,
    session_id: bytes,
    events: Callable[[str], None] | None,
) -> dict[str, Any]:
    host.secure_new(directory, directory=True)
    serial = directory.name
    macs: list[str] = []
    while len(macs) < 2:
        octets = bytearray(secrets.token_bytes(6))
        octets[0] = (octets[0] & 0xFC) | 0x02
        mac = ":".join(f"{octet:02x}" for octet in octets)
        if mac not in macs:
            macs.append(mac)
    host_mac, device_mac = macs
    private_key = directory / "id_ed25519"
    if events is not None:
        events("session-preparation")
    result = subprocess.run(
        [
            str(bundle.tools["keygen"]),
            "-q",
            "-t",
            "ed25519",
            "-N",
            "",
            "-C",
            "fplinux-ram",
            "-f",
            str(private_key),
        ],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
        creationflags=console_creation_flags(),
    )
    if result.returncode:
        fail("SSH key generation failed: " + result.stderr.strip()[-1000:])
    host.secure_new(private_key)
    public_key = private_key.with_suffix(".pub")
    host.secure_new(public_key)
    block = material.session_block(
        session_id,
        secrets.token_bytes(64),
        material.public_key_body(public_key),
        material.usb_config(serial, network, host_mac, device_mac),
    )
    image = directory / "image.bin"
    if events is not None:
        events("payload-preparation")
    material.copy_personalized_image(
        bundle.root / bundle.runtime["image"], image, bundle.runtime["personalization"], block
    )
    host.secure_new(image)
    linux_usb = bundle.runtime["usb"]["linux_gadget"]
    return {
        "target": bundle.runtime["target"],
        "session_id": session_id.hex(),
        "usb_serial": serial,
        "network": str(network),
        "host_address": str(network.network_address + 1),
        "phone_address": str(network.network_address + 2),
        "host_mac": host_mac,
        "device_mac": device_mac,
        "private_key": str(private_key),
        "known_hosts": str(directory / "known_hosts"),
        "image": str(image),
        "directory": str(directory),
        "vendor_id": linux_usb["vendor_id"],
        "product_id": linux_usb["product_id"],
        "generation": bundle.generation,
        "device_identity": bundle.device_identity,
        "status": "prepared",
        "prepared_data_sha256": None,
    }
