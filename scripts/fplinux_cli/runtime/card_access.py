# SPDX-License-Identifier: GPL-2.0-only
"""Enroll a system card from the installing user's authenticated RAM reader."""

from __future__ import annotations

import hashlib
import ipaddress
import os
import re
import secrets
import shlex
import shutil
import stat
import subprocess
from contextlib import contextmanager, suppress
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import fail, replace_file_atomically
from fplinux_cli.device_data.store import user_data_directory
from fplinux_cli.manifests.values import TARGET_NAME
from fplinux_cli.runtime.ssh import commands, material
from fplinux_cli.runtime.ssh.session import host_route_networks

from .state import exclusive_lock, private_directory

if TYPE_CHECKING:
    from collections.abc import Iterator

    from fplinux_cli.runtime.windows.host import WindowsHost

CARD_CID = re.compile(r"[0-9a-f]{32}")


def require_enrollment_path(path: Path, *, directory: bool, windows: WindowsHost | None) -> None:
    """Check ownership and mode without creating or repairing saved enrollment."""
    metadata = path.lstat()
    kind = stat.S_ISDIR if directory else stat.S_ISREG
    if not kind(metadata.st_mode):
        fail(f"Invalid private card access path: {path}")
    if windows is not None:
        windows.require_private(path)
    elif metadata.st_uid != os.getuid() or stat.S_IMODE(metadata.st_mode) != (
        0o700 if directory else 0o600
    ):
        fail(f"Card access path is not private: {path}")


def enrolled_endpoints(target: str, cid: str, windows: WindowsHost | None) -> list[dict[str, Any]]:
    """Read the complete client key, pin and USB address records for one physical card."""
    if TARGET_NAME.fullmatch(target) is None or CARD_CID.fullmatch(cid) is None:
        fail("Invalid system card access identity")
    root = user_data_directory().absolute() / "cards"
    directory = root / target / cid
    for parent in (root, directory.parent, directory):
        try:
            require_enrollment_path(parent, directory=True, windows=windows)
        except FileNotFoundError:
            fail(f"microSD {cid} is not enrolled for {target} on this PC")
    endpoints = []
    for profile in sorted(directory.iterdir()):
        if CARD_CID.fullmatch(profile.name) is None:
            continue
        try:
            require_enrollment_path(profile, directory=True, windows=windows)
            for name in ("network", "id_ed25519", "known_hosts", "ssh_config"):
                require_enrollment_path(profile / name, directory=False, windows=windows)
        except FileNotFoundError:
            # An enrollment becomes selectable only after the installer publishes its pin.
            continue
        endpoints.append(_enrollment_endpoint(target, profile))
    if not endpoints:
        fail(f"microSD {cid} has no complete SSH enrollment for {target} on this PC")
    return endpoints


def _enrollment_endpoint(target: str, directory: Path) -> dict[str, Any]:
    serial = directory.name
    pin = directory / "known_hosts"
    fields = pin.read_text(encoding="ascii").split()
    if (
        len(fields) != 3
        or fields[:2] != [f"fplinux-{serial}", "ssh-ed25519"]
        or not material.valid_ed25519_key_body(fields[2])
    ):
        fail(f"Invalid saved card SSH host key pin: {pin}")
    network = ipaddress.IPv4Network((directory / "network").read_text(encoding="ascii").strip())
    if network.prefixlen != 30 or not material.is_rfc1918(network):
        fail(f"Invalid saved card USB network: {directory / 'network'}")
    host_address, phone_address = network.hosts()
    return {
        "card_access": True,
        "target": target,
        "usb_serial": serial,
        # The card presents the same composite gadget ID as a launcher boot.
        "vendor_id": 0x0525,
        "product_id": 0xA4A6,
        "network": str(network),
        "host_address": str(host_address),
        "phone_address": str(phone_address),
        "directory": str(directory),
        "private_key": str(directory / "id_ed25519"),
        "known_hosts": str(pin),
        "host_key": fields[2],
        "status": "ready",
    }


@dataclass
class CardAccess:
    """Host configuration and public material accompanying one card write."""

    directory: Path
    network: ipaddress.IPv4Network
    ssh: Path
    usb_serial: str
    windows: WindowsHost | None
    files: tuple[Path, Path, Path]
    write_started: bool = False

    @property
    def command(self) -> str:
        """Render a shell command using the already admitted native SSH client."""
        argv = [str(self.ssh), "-F", str(self.directory / "ssh_config"), "fplinux"]
        if self.windows is not None:
            quoted = ("'" + part.replace("'", "''") + "'" for part in argv)
            return "& " + " ".join(quoted)
        return shlex.join(argv)

    def pin_host_key(self, output: str) -> str:
        """Pin the single Ed25519 public key exported by the authenticated reader."""
        body = output.strip()
        if not material.valid_ed25519_key_body(body):
            fail("The phone did not provide a valid card SSH host key")
        known_hosts = self.directory / "known_hosts"
        _write_private(
            known_hosts, f"fplinux-{self.usb_serial} ssh-ed25519 {body}\n".encode(), self.windows
        )
        host_address, phone_address = self.network.hosts()
        config = commands.ssh_config(
            {
                "private_key": (self.directory / "id_ed25519").as_posix().replace("%", "%%"),
                "known_hosts": known_hosts.as_posix().replace("%", "%%"),
                "usb_serial": self.usb_serial,
                "host_address": str(host_address),
                "phone_address": str(phone_address),
            },
            global_known_hosts="NUL" if self.windows is not None else "/dev/null",
        )
        _write_private(self.directory / "ssh_config", config.encode(), self.windows)
        if self.windows is None:
            descriptor = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        return body


def _write_private(path: Path, data: bytes, windows: WindowsHost | None) -> None:
    replace_file_atomically(path, data, 0o600)
    if windows is not None:
        windows.secure_new(path)


def _saved_networks(root: Path) -> set[ipaddress.IPv4Network]:
    networks = set()
    for path in root.glob("*/*/*/network"):
        try:
            networks.add(ipaddress.IPv4Network(path.read_text(encoding="ascii").strip()))
        except FileNotFoundError:
            # An earlier installation can finish its cleanup after this glob.
            continue
        except ValueError:
            fail(f"Invalid saved card network in {path}")
    return networks


def _generate_client_key(path: Path, keygen: Path, windows: WindowsHost | None) -> bytes:
    creationflags = 0
    if windows is not None:
        from fplinux_cli.runtime.windows.host import console_creation_flags  # noqa: PLC0415

        creationflags = console_creation_flags()
    result = subprocess.run(
        [str(keygen), "-q", "-t", "ed25519", "-N", "", "-C", "fplinux-card", "-f", str(path)],
        capture_output=True,
        check=False,
        timeout=20,
        creationflags=creationflags,
    )
    if result.returncode:
        fail("Cannot generate the card SSH client key")
    if windows is None:
        path.chmod(0o600)
    else:
        windows.secure_new(path)
    with path.open("r+b") as private_key:
        os.fsync(private_key.fileno())
    public_key = path.with_suffix(".pub")
    body = material.public_key_body(public_key)
    public_key.unlink()
    return body


@contextmanager
def prepare_card_access(
    connection: tuple[Any, dict[str, Any]], target: str, cid: str, local: Path
) -> Iterator[CardAccess]:
    """Keep previous card profiles and retain the new key once the writer starts."""
    if TARGET_NAME.fullmatch(target) is None or CARD_CID.fullmatch(cid) is None:
        fail("Invalid system card access identity")
    transport, session = connection
    windows: WindowsHost | None = None
    if os.name == "nt":
        windows = transport.host
        ssh, keygen = transport.tools["ssh"], transport.tools["keygen"]
    else:
        ssh = Path(commands.require_tool("ssh")).absolute()
        keygen = Path(commands.require_tool("ssh-keygen")).absolute()
    protect_directory = windows.runtime_root if windows is not None else private_directory
    root = protect_directory(user_data_directory().absolute() / "cards")
    card = protect_directory(protect_directory(root / target) / cid)
    device_id = secrets.token_bytes(32)
    usb_serial = hashlib.sha256(device_id).hexdigest()[:32]
    directory = card / usb_serial
    access = None
    created = False
    try:
        with exclusive_lock(
            root / ".install.lock", message="Another card is enrolling on this PC"
        ):
            occupied = windows.route_networks() if windows is not None else host_route_networks()
            occupied.add(ipaddress.IPv4Network(session["network"]))
            network = material.allocate_network(occupied | _saved_networks(root))
            directory.mkdir(mode=0o700)
            created = True
            protect_directory(directory)
            _write_private(directory / "network", f"{network}\n".encode(), windows)
        client_key = _generate_client_key(directory / "id_ed25519", keygen, windows)
        host_mac, device_mac = material.mac_pair()
        network_config = material.usb_config(
            usb_serial,
            network,
            host_mac,
            device_mac,
            transport="rndis" if windows is not None else "ncm",
        )
        device_path = local / "device-id"
        network_path = local / "usb-network"
        key_path = local / "client-key"
        for path, data in (
            (device_path, device_id),
            (network_path, network_config),
            (key_path, client_key),
        ):
            _write_private(path, data, windows)
        access = CardAccess(
            directory, network, ssh, usb_serial, windows, (device_path, network_path, key_path)
        )
        yield access
    finally:
        if created and (access is None or not access.write_started):
            with suppress(OSError):
                shutil.rmtree(directory)
