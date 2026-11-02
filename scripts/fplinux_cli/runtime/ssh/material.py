# SPDX-License-Identifier: GPL-2.0-only
"""Shared SSH session material and image personalization."""

from __future__ import annotations

import base64
import binascii
import hashlib
import ipaddress
import os
import shutil
import struct
import zlib
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import sha256_file

from .errors import fail

if TYPE_CHECKING:
    from pathlib import Path

SESSION_MAGIC = b"FPLSESS\0"
SESSION_BYTES = 512
SESSION_CRC_OFFSET = 508
RFC1918_NETWORKS = tuple(
    ipaddress.IPv4Network(value) for value in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
)


def is_rfc1918(network: ipaddress.IPv4Network) -> bool:
    return any(network.subnet_of(private) for private in RFC1918_NETWORKS)


def candidate_network(random_bytes: bytes) -> ipaddress.IPv4Network:
    """Map random bytes uniformly enough across all three RFC 1918 blocks."""
    selector = random_bytes[0] % 3
    value = int.from_bytes(random_bytes[1:5], "big")
    if selector == 0:
        base, subnet_count = int(ipaddress.IPv4Address("10.0.0.0")), 1 << 22
    elif selector == 1:
        base, subnet_count = int(ipaddress.IPv4Address("172.16.0.0")), 1 << 18
    else:
        base, subnet_count = int(ipaddress.IPv4Address("192.168.0.0")), 1 << 14
    return ipaddress.IPv4Network((base + (value % subnet_count) * 4, 30))


def public_key_body(public_key: Path) -> bytes:
    try:
        fields = public_key.read_text(encoding="ascii").strip().split()
    except (OSError, UnicodeDecodeError) as error:
        fail(f"cannot read generated SSH public key: {error}")
    if len(fields) < 2 or fields[0] != "ssh-ed25519" or len(fields[1]) != 68:
        fail("ssh-keygen did not produce the required Ed25519 public key")
    try:
        decoded = base64.b64decode(fields[1], validate=True)
    except binascii.Error:
        fail("ssh-keygen produced invalid public-key base64")
    if decoded[:15] != b"\0\0\0\x0bssh-ed25519" or decoded[15:19] != b"\0\0\0\x20":
        fail("ssh-keygen produced an unexpected Ed25519 public-key blob")
    if len(decoded) != 51:
        fail("ssh-keygen produced an unexpected Ed25519 public-key size")
    return fields[1].encode("ascii")


def usb_config(
    usb_serial: str,
    network: ipaddress.IPv4Network,
    host_mac: str,
    device_mac: str,
) -> bytes:
    hosts = list(network.hosts())
    host_address, phone_address = hosts[0], hosts[1]
    text = "".join(
        (
            f"usb_serial={usb_serial}\n",
            f"phone_address={phone_address}\n",
            f"host_address={host_address}\n",
            f"netmask={network.netmask}\n",
            f"broadcast={network.broadcast_address}\n",
            f"device_mac={device_mac}\n",
            f"host_mac={host_mac}\n",
        )
    ).encode("ascii")
    if len(text) > 256:
        fail("USB session configuration exceeds its fixed field")
    return text.ljust(256, b"\0")


def session_block(
    session_id: bytes,
    rng_seed: bytes,
    public_key: bytes,
    usb_config: bytes,
) -> bytes:
    if (
        len(session_id) != 32
        or len(rng_seed) != 64
        or len(public_key) != 68
        or len(usb_config) != 256
    ):
        fail("session material does not match the fixed ABI")
    record = bytearray(SESSION_BYTES)
    record[:8] = SESSION_MAGIC
    struct.pack_into("<I", record, 12, SESSION_BYTES)
    record[16:48] = session_id
    record[48:112] = rng_seed
    record[112:180] = public_key
    record[180:436] = usb_config
    struct.pack_into("<I", record, SESSION_CRC_OFFSET, zlib.crc32(record[:508]))
    return bytes(record)


def copy_personalized_image(
    source: Path,
    destination: Path,
    descriptor: dict[str, Any],
    block: bytes,
) -> None:
    if source.is_symlink() or not source.is_file():
        fail(f"RAM image is missing or invalid: {source}")
    offset = descriptor["offset"]
    expected = descriptor["template_sha256"]
    source_digest = sha256_file(source)
    with source.open("rb") as stream:
        stream.seek(offset)
        template = stream.read(SESSION_BYTES)
    if len(template) != SESSION_BYTES or hashlib.sha256(template).hexdigest() != expected:
        fail("RAM image personalization template does not match its manifest")
    shutil.copyfile(source, destination)
    destination.chmod(0o600)
    with destination.open("r+b") as stream:
        stream.seek(offset)
        stream.write(block)
        stream.flush()
        os.fsync(stream.fileno())
    if sha256_file(source) != source_digest:
        destination.unlink(missing_ok=True)
        fail("canonical RAM image changed while the session copy was prepared")
    with source.open("rb") as canonical, destination.open("rb") as personalized:
        before = canonical.read(offset)
        if personalized.read(offset) != before:
            fail("personalized image differs before its declared session slot")
        canonical.seek(offset + SESSION_BYTES)
        personalized.seek(offset + SESSION_BYTES)
        while True:
            expected_chunk = canonical.read(1024 * 1024)
            actual_chunk = personalized.read(1024 * 1024)
            if actual_chunk != expected_chunk:
                fail("personalized image differs outside its declared session slot")
            if not expected_chunk:
                break
