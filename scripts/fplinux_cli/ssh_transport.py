# SPDX-License-Identifier: GPL-2.0-only
"""Bind one volatile RAM image to an isolated USB-NCM SSH session."""

from __future__ import annotations

import base64
import binascii
import contextlib
import hashlib
import hmac
import ipaddress
import json
import math
import os
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path, PurePosixPath
from typing import Any, NoReturn

SESSION_OWNER_KIND = "fplinux-host-session-owner"
SESSION_MAGIC = b"FPLSESS\0"
SESSION_BYTES = 512
SESSION_CRC_OFFSET = 508
SSH_PORT = 22
REMOTE_PATH = re.compile(r"/(?:[A-Za-z0-9._-]+/)*[A-Za-z0-9._-]+")
TARGET_NAME = re.compile(r"[a-z0-9][a-z0-9._-]*")
SHA256 = re.compile(r"[0-9a-f]{64}")
BUNDLE_IDENTITY_FIELDS = frozenset({"bundle_generation"})
BUILD_MANIFEST_FIELDS = frozenset(
    {
        "rootfs_receipt",
        "boot_artifacts",
        "container_image_recipe",
        "apk_signing_key",
        "device_identity",
        "files",
        "generation",
        "kbuild_receipt",
        "linux_recipe",
        "profile",
        "target",
        "workspace_digest",
    }
)
RFC1918_NETWORKS = tuple(
    ipaddress.IPv4Network(value) for value in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
)


def fail(message: str) -> NoReturn:
    """Stop without falling back to a less-bound transport."""
    raise SystemExit(f"SSH transport failed: {message}")


def _require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        fail(f"required host tool is missing: {name}")
    return path


def _private_directory(path: Path) -> Path:
    """Create or validate one user-owned mode-0700 runtime directory."""
    with contextlib.suppress(FileExistsError):
        path.mkdir(mode=0o700)
    try:
        metadata = path.lstat()
    except OSError as error:
        fail(f"cannot inspect runtime directory {path}: {error}")
    if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        fail(f"runtime path is not a real directory: {path}")
    if metadata.st_uid != os.getuid():
        fail(f"runtime directory is not owned by the current user: {path}")
    if stat.S_IMODE(metadata.st_mode) != 0o700:
        try:
            path.chmod(0o700)
        except OSError as error:
            fail(f"cannot protect runtime directory {path}: {error}")
    return path


def _runtime_root() -> Path:
    value = os.environ.get("XDG_RUNTIME_DIR")
    if not value:
        fail("XDG_RUNTIME_DIR is not set")
    base = Path(value)
    if not base.is_absolute():
        fail("XDG_RUNTIME_DIR must be absolute")
    try:
        metadata = base.lstat()
    except OSError as error:
        fail(f"XDG_RUNTIME_DIR is unavailable: {error}")
    if (
        not stat.S_ISDIR(metadata.st_mode)
        or stat.S_ISLNK(metadata.st_mode)
        or metadata.st_uid != os.getuid()
    ):
        fail("XDG_RUNTIME_DIR must be a real directory owned by the current user")
    return _private_directory(base / "fplinux")


def _write_json(path: Path, value: dict[str, Any]) -> None:
    _write_private_text(path, json.dumps(value, sort_keys=True) + "\n")


def _write_private_text(path: Path, value: str) -> None:
    """Atomically replace one mode-0600 runtime text file."""
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=path.parent,
            prefix=f".{path.name}.",
            mode="w",
            encoding="utf-8",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(0o600)
        temporary.replace(path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _sha256_file(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def _canonical_json_bytes(value: object) -> bytes:
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
    ).encode()


def _regular_file_bytes(path: Path, name: str) -> bytes:
    try:
        metadata = path.lstat()
        data = path.read_bytes()
    except OSError as error:
        fail(f"cannot read {name}: {error}")
    if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        fail(f"{name} is not a regular file: {path}")
    return data


def _file_record(manifest: dict[str, Any], relative: str) -> dict[str, Any]:
    files = manifest.get("files")
    record = files.get(relative) if isinstance(files, dict) else None
    if (
        not isinstance(record, dict)
        or set(record) != {"mode", "sha256", "size"}
        or type(record.get("mode")) is not int
        or type(record.get("size")) is not int
        or not isinstance(record.get("sha256"), str)
        or SHA256.fullmatch(record["sha256"]) is None
    ):
        fail(f"build manifest has no valid record for {relative}")
    return record


def bundle_identity(bundle: Path, runtime: dict[str, Any]) -> dict[str, str]:
    """Return the immutable build identity that created this runtime closure."""
    runtime_path = bundle / "runtime-manifest.json"
    runtime_bytes = _regular_file_bytes(runtime_path, "runtime manifest")
    target = runtime.get("target")
    profile = runtime.get("profile")
    image_relative = runtime.get("image")
    hashes = runtime.get("sha256")
    if (
        not isinstance(target, str)
        or TARGET_NAME.fullmatch(target) is None
        or (
            profile is not None
            and (not isinstance(profile, str) or TARGET_NAME.fullmatch(profile) is None)
        )
        or not isinstance(image_relative, str)
        or not isinstance(hashes, dict)
    ):
        fail("runtime manifest does not describe one SSH bundle")
    image_name = PurePosixPath(image_relative)
    if (
        image_name.is_absolute()
        or ".." in image_name.parts
        or image_name.as_posix() != image_relative
    ):
        fail("runtime image path is not a normalized bundle-relative path")

    manifest_path = bundle / "build-manifest.json"
    manifest_bytes = _regular_file_bytes(manifest_path, "build manifest")
    try:
        manifest = json.loads(manifest_bytes)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"build manifest is invalid: {error}")
    if (
        not isinstance(manifest, dict)
        or set(manifest) != BUILD_MANIFEST_FIELDS
        or manifest.get("target") != target
        or manifest.get("profile") != profile
    ):
        fail("build manifest does not match the runtime target or profile")
    generation = manifest.get("generation")
    if not isinstance(generation, str) or SHA256.fullmatch(generation) is None:
        fail("build manifest generation is invalid")
    payload = {key: value for key, value in manifest.items() if key != "generation"}
    if hashlib.sha256(_canonical_json_bytes(payload)).hexdigest() != generation:
        fail("build manifest generation does not match its payload")

    image = bundle / image_relative
    runtime_record = _file_record(manifest, "runtime-manifest.json")
    image_record = _file_record(manifest, image_relative)
    if (
        runtime_record["sha256"] != hashlib.sha256(runtime_bytes).hexdigest()
        or runtime_record["size"] != len(runtime_bytes)
        or image_record["sha256"] != _sha256_file(image)
        or image_record["size"] != image.stat().st_size
        or hashes.get(image_relative) != image_record["sha256"]
    ):
        fail("runtime closure differs from its immutable build manifest")
    return {
        "bundle_generation": generation,
    }


def load_bundle_context(bundle: Path) -> tuple[dict[str, Any], dict[str, str]]:
    """Load the minimum standalone reconnect context from an extracted bundle."""
    data = _regular_file_bytes(bundle / "runtime-manifest.json", "runtime manifest")
    try:
        runtime = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"runtime manifest is invalid: {error}")
    if not isinstance(runtime, dict):
        fail("runtime manifest root is not an object")
    return runtime, bundle_identity(bundle, runtime)


def _validate_bundle_identity(value: object) -> dict[str, str]:
    if not isinstance(value, dict) or set(value) != BUNDLE_IDENTITY_FIELDS:
        fail("bundle identity has unexpected fields")
    for field in BUNDLE_IDENTITY_FIELDS:
        digest = value.get(field)
        if not isinstance(digest, str) or SHA256.fullmatch(digest) is None:
            fail(f"bundle identity {field} is invalid")
    return value


def _host_route_networks(ip_tool: str) -> set[ipaddress.IPv4Network]:
    result = subprocess.run(
        [ip_tool, "-4", "-j", "route", "show", "table", "all"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode:
        detail = result.stderr.strip().splitlines()
        fail("cannot inspect host IPv4 routes" + (f": {detail[-1]}" if detail else ""))
    try:
        routes = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        fail(f"host IPv4 route inventory is invalid: {error}")
    if not isinstance(routes, list):
        fail("host IPv4 route inventory is not a list")
    networks: set[ipaddress.IPv4Network] = set()
    for route in routes:
        if not isinstance(route, dict):
            fail("host IPv4 route entry is invalid")
        destination = route.get("dst")
        if destination in (None, "default"):
            continue
        if not isinstance(destination, str):
            fail("host IPv4 route destination is invalid")
        try:
            networks.add(ipaddress.IPv4Network(destination, strict=False))
        except ValueError as error:
            fail(f"host IPv4 route destination is invalid: {error}")
    return networks


def _active_session_networks(sessions: Path) -> set[ipaddress.IPv4Network]:
    networks: set[ipaddress.IPv4Network] = set()
    for state_path in sessions.glob("*/session.json"):
        if state_path.is_symlink() or not state_path.is_file():
            continue
        try:
            state = json.loads(state_path.read_text(encoding="utf-8"))
            if not isinstance(state, dict):
                continue
            network = ipaddress.IPv4Network(state.get("network"), strict=True)
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
            continue
        if network.prefixlen == 30 and _is_rfc1918(network):
            networks.add(network)
    return networks


def _is_rfc1918(network: ipaddress.IPv4Network) -> bool:
    return any(network.subnet_of(private) for private in RFC1918_NETWORKS)


def _candidate_network(random_bytes: bytes) -> ipaddress.IPv4Network:
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


def _choose_network(sessions: Path) -> ipaddress.IPv4Network:
    occupied = _host_route_networks(_require_tool("ip")) | _active_session_networks(sessions)
    for _attempt in range(4096):
        candidate = _candidate_network(os.getrandom(5))
        if all(not candidate.overlaps(network) for network in occupied):
            return candidate
    fail("could not allocate a free private USB /30")


def _mac_address() -> str:
    octets = bytearray(os.getrandom(6))
    octets[0] = (octets[0] & 0xFC) | 0x02
    return ":".join(f"{octet:02x}" for octet in octets)


def _mac_pair() -> tuple[str, str]:
    host = _mac_address()
    device = _mac_address()
    while device == host:
        device = _mac_address()
    return host, device


def _public_key_body(public_key: Path) -> bytes:
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


def _usb_config(
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


def _session_block(
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


def _copy_personalized_image(
    source: Path,
    destination: Path,
    descriptor: dict[str, Any],
    block: bytes,
) -> None:
    if source.is_symlink() or not source.is_file():
        fail(f"RAM image is missing or invalid: {source}")
    offset = descriptor["offset"]
    expected = descriptor["template_sha256"]
    source_digest = _sha256_file(source)
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
    if _sha256_file(source) != source_digest:
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


def _session_directory(root: Path, target: str, usb_serial: str) -> Path:
    if TARGET_NAME.fullmatch(target) is None or re.fullmatch(r"[0-9a-f]{32}", usb_serial) is None:
        fail("session directory identity is invalid")
    return root / "sessions" / f"{target}.{usb_serial}"


def _remove_owned_session(directory: Path, target: str, usb_serial: str) -> None:
    """Remove only a session directory carrying our exact private owner record."""
    expected = _session_directory(_runtime_root(), target, usb_serial)
    if directory != expected:
        fail("refusing to remove a session directory outside the runtime root")
    try:
        metadata = directory.lstat()
    except FileNotFoundError:
        return
    except OSError as error:
        fail(f"cannot inspect session directory {directory}: {error}")
    if (
        not stat.S_ISDIR(metadata.st_mode)
        or stat.S_ISLNK(metadata.st_mode)
        or metadata.st_uid != os.getuid()
    ):
        fail(f"refusing to remove an invalid session directory: {directory}")
    owner_path = directory / "owner.json"
    try:
        owner = json.loads(_regular_file_bytes(owner_path, "session owner record"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"session owner record is invalid: {error}")
    if owner != {
        "kind": SESSION_OWNER_KIND,
        "target": target,
        "usb_serial": usb_serial,
    }:
        fail(f"refusing to remove an unrecognized session directory: {directory}")
    shutil.rmtree(directory)


def _cleanup_target_sessions(root: Path, target: str) -> None:
    """Invalidate the prior target pointer and remove only owned superseded sessions."""
    current_directory = _private_directory(root / "current")
    for suffix, label in ((".json", "session"), (".ssh-config", "configuration")):
        current = current_directory / f"{target}{suffix}"
        if current.is_symlink():
            current.unlink()
        elif current.exists() and current.is_dir():
            fail(f"current SSH {label} path is a directory: {current}")
        else:
            current.unlink(missing_ok=True)

    sessions = _private_directory(root / "sessions")
    for directory in sessions.glob(f"{target}.*"):
        if directory.is_symlink() or not directory.is_dir():
            continue
        owner_path = directory / "owner.json"
        try:
            owner = json.loads(_regular_file_bytes(owner_path, "session owner record"))
        except SystemExit:
            continue
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        if (
            isinstance(owner, dict)
            and owner.get("kind") == SESSION_OWNER_KIND
            and owner.get("target") == target
            and isinstance(owner.get("usb_serial"), str)
            and directory == _session_directory(root, target, owner["usb_serial"])
        ):
            _remove_owned_session(directory, target, owner["usb_serial"])


def prepare_session(
    image: Path,
    personalization: dict[str, Any],
    target: str,
    linux_usb: dict[str, Any],
    identity: dict[str, str],
) -> dict[str, Any]:
    """Create keys, addressing and one mode-0600 personalized image copy."""
    if TARGET_NAME.fullmatch(target) is None:
        fail(f"invalid target name: {target}")
    identity = _validate_bundle_identity(identity)
    root = _runtime_root()
    _cleanup_target_sessions(root, target)
    for tool in ("ip", "ssh", "ssh-keygen", "ssh-keyscan", "sftp"):
        _require_tool(tool)
    if set(personalization) != {"offset", "bytes", "template_sha256"}:
        fail("personalization descriptor has unexpected fields")
    offset = personalization.get("offset")
    if type(offset) is not int or offset < 512 or offset % 64:
        fail("personalization offset must be a 64-byte-aligned image offset")
    if personalization.get("bytes") != SESSION_BYTES:
        fail(f"personalization size must be {SESSION_BYTES} bytes")
    template_hash = personalization.get("template_sha256")
    if not isinstance(template_hash, str) or SHA256.fullmatch(template_hash) is None:
        fail("personalization template_sha256 must be a lowercase SHA-256 digest")
    if offset + SESSION_BYTES > image.stat().st_size:
        fail("personalization slot extends past the RAM image")

    sessions = _private_directory(root / "sessions")
    session_id = os.getrandom(32)
    rng_seed = os.getrandom(64)
    usb_serial = hashlib.sha256(session_id).hexdigest()[:32]
    session_directory = _session_directory(root, target, usb_serial)
    try:
        session_directory.mkdir(mode=0o700)
    except FileExistsError:
        fail("random USB session identifier collided with existing runtime state")
    _private_directory(session_directory)
    try:
        _write_json(
            session_directory / "owner.json",
            {
                "kind": SESSION_OWNER_KIND,
                "target": target,
                "usb_serial": usb_serial,
            },
        )
    except BaseException:
        shutil.rmtree(session_directory)
        raise

    private_key = session_directory / "client_ed25519"
    public_key = session_directory / "client_ed25519.pub"
    known_hosts = session_directory / "known_hosts"
    state_path = session_directory / "session.json"
    personalized_image = session_directory / "ramboot.bin"
    try:
        network = _choose_network(sessions)
        host_mac, device_mac = _mac_pair()
        keygen = _require_tool("ssh-keygen")
        result = subprocess.run(
            [
                keygen,
                "-q",
                "-t",
                "ed25519",
                "-N",
                "",
                "-C",
                f"fplinux-session:{usb_serial}",
                "-f",
                str(private_key),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode:
            detail = result.stderr.strip().splitlines()
            fail("ssh-keygen failed" + (f": {detail[-1]}" if detail else ""))
        private_key.chmod(0o600)
        public_key.chmod(0o600)
        key_body = _public_key_body(public_key)
        block = _session_block(
            session_id,
            rng_seed,
            key_body,
            _usb_config(usb_serial, network, host_mac, device_mac),
        )
        _copy_personalized_image(image, personalized_image, personalization, block)
    except BaseException:
        _remove_owned_session(session_directory, target, usb_serial)
        raise

    host_address, phone_address = (str(address) for address in network.hosts())
    state: dict[str, Any] = {
        "target": target,
        "session_id": session_id.hex(),
        "usb_serial": usb_serial,
        "network": str(network),
        "host_address": host_address,
        "phone_address": phone_address,
        "host_mac": host_mac,
        "device_mac": device_mac,
        "private_key": str(private_key),
        "known_hosts": str(known_hosts),
        "image": str(personalized_image),
        "vendor_id": linux_usb["vendor_id"],
        "product_id": linux_usb["product_id"],
        "wait_seconds": linux_usb["wait_seconds"],
        "status": "prepared",
        **identity,
    }
    try:
        _write_json(state_path, state)
    except BaseException:
        _remove_owned_session(session_directory, target, usb_serial)
        raise
    return state


def remove_personalized_image(session: dict[str, Any]) -> None:
    """Remove the secret-bearing image as soon as the RAM loader returns."""
    target = session.get("target")
    usb_serial = session.get("usb_serial")
    image = session.get("image")
    if (
        not isinstance(target, str)
        or not isinstance(usb_serial, str)
        or not isinstance(image, str)
    ):
        fail("personalized image identity is invalid")
    path = Path(image)
    expected = _session_directory(_runtime_root(), target, usb_serial) / "ramboot.bin"
    if path != expected:
        fail("refusing to remove a personalized image outside its session directory")
    path.unlink(missing_ok=True)
    if path.exists() or path.is_symlink():
        fail(f"could not remove personalized RAM image: {path}")


def finish_session(session: dict[str, Any]) -> None:
    """Retain a ready reconnect session; erase keys from any incomplete attempt."""
    remove_personalized_image(session)
    target = session.get("target")
    usb_serial = session.get("usb_serial")
    if not isinstance(target, str) or not isinstance(usb_serial, str):
        fail("cannot finish a session with invalid identity")
    directory = _session_directory(_runtime_root(), target, usb_serial)
    state_path = directory / "session.json"
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        state = None
    ready = False
    if isinstance(state, dict) and state.get("status") == "ready":
        try:
            _validate_session(state)
        except SystemExit:
            pass
        else:
            ready = True
    if not ready:
        _remove_owned_session(directory, target, usb_serial)


def _usb_devices(session: dict[str, Any]) -> list[Path]:
    matches: list[Path] = []
    root = Path("/sys/bus/usb/devices")
    for vendor_file in root.glob("*/idVendor"):
        device = vendor_file.parent
        try:
            vendor = int(vendor_file.read_text(encoding="ascii").strip(), 16)
            product = int(
                (device / "idProduct").read_text(encoding="ascii").strip(),
                16,
            )
            serial = (device / "serial").read_text(encoding="ascii").strip()
        except (FileNotFoundError, PermissionError, UnicodeDecodeError, ValueError):
            continue
        if (
            vendor == session["vendor_id"]
            and product == session["product_id"]
            and hmac.compare_digest(serial, session["usb_serial"])
        ):
            matches.append(device.resolve())
    return matches


def _driver_is_cdc_ncm(path: Path, usb_device: Path) -> bool:
    current = path
    while current != usb_device.parent:
        driver = current / "driver"
        try:
            if driver.resolve().name == "cdc_ncm":
                return True
        except (FileNotFoundError, OSError):
            pass
        if current == usb_device:
            break
        current = current.parent
    return False


def _ncm_interface(usb_device: Path, expected_mac: str) -> str | None:
    matches: list[str] = []
    for netdev in Path("/sys/class/net").iterdir():
        try:
            device = (netdev / "device").resolve(strict=True)
            address = (netdev / "address").read_text(encoding="ascii").strip().lower()
        except (FileNotFoundError, PermissionError, UnicodeDecodeError, OSError):
            continue
        if usb_device not in (device, *device.parents):
            continue
        if address != expected_mac or not _driver_is_cdc_ncm(device, usb_device):
            continue
        matches.append(netdev.name)
    if len(matches) > 1:
        fail("more than one matching cdc_ncm interface belongs to the RAM session")
    return matches[0] if matches else None


def _ip_json(arguments: list[str]) -> object | None:
    result = subprocess.run(
        [_require_tool("ip"), "-4", "-j", *arguments],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode:
        return None
    try:
        decoded: object = json.loads(result.stdout)
    except json.JSONDecodeError:
        return None
    return decoded


def _network_ready(interface: str, session: dict[str, Any]) -> bool:
    addresses = _ip_json(["address", "show", "dev", interface])
    if not isinstance(addresses, list) or len(addresses) != 1:
        return False
    infos = addresses[0].get("addr_info") if isinstance(addresses[0], dict) else None
    if not isinstance(infos, list) or not any(
        isinstance(info, dict)
        and info.get("family") == "inet"
        and info.get("local") == session["host_address"]
        and info.get("prefixlen") == 30
        for info in infos
    ):
        return False
    routes = _ip_json(["route", "show", session["network"]])
    if not isinstance(routes, list):
        return False
    return any(
        isinstance(route, dict)
        and route.get("dst") == session["network"]
        and route.get("dev") == interface
        for route in routes
    )


def _ssh_option_values(
    session: dict[str, Any],
    *,
    connect_timeout: int = 5,
) -> list[tuple[str, str]]:
    """Return the client policy shared by bundled and direct OpenSSH use."""
    alias = f"fplinux-{session['usb_serial']}"
    return [
        ("BatchMode", "yes"),
        ("IdentitiesOnly", "yes"),
        ("IdentityAgent", "none"),
        ("PasswordAuthentication", "no"),
        ("KbdInteractiveAuthentication", "no"),
        ("PreferredAuthentications", "publickey"),
        ("NumberOfPasswordPrompts", "0"),
        ("IdentityFile", str(session["private_key"])),
        ("StrictHostKeyChecking", "yes"),
        ("UserKnownHostsFile", str(session["known_hosts"])),
        ("GlobalKnownHostsFile", "/dev/null"),
        ("HostKeyAlias", alias),
        ("CheckHostIP", "no"),
        ("ClearAllForwardings", "yes"),
        ("ForwardAgent", "no"),
        ("ForwardX11", "no"),
        ("Tunnel", "no"),
        ("PermitLocalCommand", "no"),
        ("ProxyCommand", "none"),
        ("ProxyJump", "none"),
        ("BindAddress", str(session["host_address"])),
        ("ConnectTimeout", str(connect_timeout)),
        ("ConnectionAttempts", "1"),
        ("LogLevel", "ERROR"),
    ]


def _ssh_options(session: dict[str, Any], *, connect_timeout: int = 5) -> list[str]:
    return [
        "-F",
        "/dev/null",
        *(
            option
            for name, value in _ssh_option_values(session, connect_timeout=connect_timeout)
            for option in ("-o", f"{name}={value}")
        ),
    ]


def _ssh_config_value(value: str) -> str:
    """Quote an OpenSSH configuration argument without allowing a new directive."""
    if any(character in value for character in ("\0", "\n", "\r")):
        fail("SSH configuration value is not a single line")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _ssh_config_directive(name: str, value: str) -> str:
    """Render one directive, preserving OpenSSH's hard-coded ``none`` sentinels."""
    literal_none = {"IdentityAgent", "ProxyCommand", "ProxyJump"}
    rendered = "none" if name in literal_none and value == "none" else _ssh_config_value(value)
    return f"    {name} {rendered}\n"


def _ssh_config(session: dict[str, Any]) -> str:
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


def _ssh_argv(
    session: dict[str, Any],
    command: str | None = None,
    *,
    connect_timeout: int = 5,
) -> list[str]:
    argv = [
        _require_tool("ssh"),
        *_ssh_options(session, connect_timeout=connect_timeout),
        f"root@{session['phone_address']}",
    ]
    if command is not None:
        argv.append(command)
    return argv


def _scan_host_key(session: dict[str, Any], timeout: int) -> bool:
    scanner = _require_tool("ssh-keyscan")
    result = subprocess.run(
        [
            scanner,
            "-4",
            "-T",
            str(timeout),
            "-p",
            str(SSH_PORT),
            "-t",
            "ed25519",
            session["phone_address"],
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    lines = [line for line in result.stdout.splitlines() if line and not line.startswith("#")]
    keys = {tuple(line.split()[1:3]) for line in lines if len(line.split()) == 3}
    if result.returncode or len(keys) != 1:
        return False
    key_type, key_body = next(iter(keys))
    if key_type != "ssh-ed25519":
        return False
    try:
        decoded = base64.b64decode(key_body, validate=True)
    except binascii.Error:
        return False
    if (
        len(decoded) != 51
        or decoded[:15] != b"\0\0\0\x0bssh-ed25519"
        or decoded[15:19] != b"\0\0\0\x20"
    ):
        return False
    alias = f"fplinux-{session['usb_serial']}"
    known_hosts = Path(session["known_hosts"])
    known_hosts.write_text(f"{alias} {key_type} {key_body}\n", encoding="ascii")
    known_hosts.chmod(0o600)
    return True


def _mark_current(session: dict[str, Any]) -> dict[str, Any]:
    session = dict(session)
    session["status"] = "ready"
    target = session.get("target")
    usb_serial = session.get("usb_serial")
    if not isinstance(target, str) or not isinstance(usb_serial, str):
        fail("cannot publish a session with invalid identity")
    directory = Path(session["private_key"]).parent
    expected = _session_directory(_runtime_root(), target, usb_serial)
    if directory != expected:
        fail("cannot publish a session outside its runtime directory")
    current = _private_directory(_runtime_root() / "current")
    config_path = current / f"{target}.ssh-config"
    try:
        _write_json(directory / "session.json", session)
        _write_json(current / f"{target}.json", session)
        _write_private_text(config_path, _ssh_config(session))
    except BaseException:
        with contextlib.suppress(OSError):
            (current / f"{target}.json").unlink(missing_ok=True)
            config_path.unlink(missing_ok=True)
        _remove_owned_session(directory, target, usb_serial)
        raise
    return session


def _retry_pause(deadline: float) -> None:
    remaining = deadline - time.monotonic()
    if remaining > 0:
        time.sleep(min(0.25, remaining))


def _wait_for_authenticated_endpoint(
    session: dict[str, Any],
    *,
    scan_host_key: bool,
) -> dict[str, Any]:
    """Wait for the exact USB path and an authenticated session-id response."""
    deadline = time.monotonic() + session["wait_seconds"]
    announced_key = False
    while time.monotonic() < deadline:
        devices = _usb_devices(session)
        if len(devices) > 1:
            fail("more than one USB device matches this RAM session")
        interface = _ncm_interface(devices[0], session["host_mac"]) if devices else None
        if interface is None or not _network_ready(interface, session):
            _retry_pause(deadline)
            continue

        remaining = max(1, math.ceil(deadline - time.monotonic()))
        timeout = min(5, remaining)
        candidate = {**session, "interface": interface}
        if scan_host_key:
            if not _scan_host_key(candidate, timeout):
                _retry_pause(deadline)
                continue
            if not announced_key:
                print("SSH host key observed; validating the private RAM session identity.")
                announced_key = True
        result = subprocess.run(
            _ssh_argv(candidate, "fplinux-session-id", connect_timeout=timeout),
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode:
            _retry_pause(deadline)
            continue
        actual = result.stdout.strip()
        if not hmac.compare_digest(actual, session["session_id"]):
            fail("SSH endpoint proved a different RAM session identity")
        return _mark_current(candidate)
    if scan_host_key:
        fail("the exact USB-NCM SSH session did not become ready before the deadline")
    fail("the current USB-NCM SSH session did not reconnect before the deadline")


def wait_for_bound_session(session: dict[str, Any]) -> dict[str, Any]:
    """Wait for exact USB/NCM state, then bind the observed host key to this RAM run."""
    return _wait_for_authenticated_endpoint(session, scan_host_key=True)


def _validate_session(
    value: object,
    target: str | None = None,
    identity: dict[str, str] | None = None,
) -> dict[str, Any]:
    required = {
        "target",
        "session_id",
        "usb_serial",
        "network",
        "host_address",
        "phone_address",
        "host_mac",
        "device_mac",
        "private_key",
        "known_hosts",
        "image",
        "vendor_id",
        "product_id",
        "wait_seconds",
        "status",
        "interface",
        *BUNDLE_IDENTITY_FIELDS,
    }
    if not isinstance(value, dict) or set(value) != required:
        fail("current SSH session state has unexpected fields")
    if value.get("status") != "ready":
        fail("current SSH session state is not ready")
    if target is not None and value.get("target") != target:
        fail("current SSH session belongs to another target")
    if identity is not None:
        identity = _validate_bundle_identity(identity)
        if any(value.get(field) != identity[field] for field in BUNDLE_IDENTITY_FIELDS):
            fail("current SSH session belongs to a stale bundle; load the current build")
    _validate_bundle_identity({field: value.get(field) for field in BUNDLE_IDENTITY_FIELDS})
    session_id = value.get("session_id")
    if (
        not isinstance(session_id, str)
        or SHA256.fullmatch(session_id) is None
        or hashlib.sha256(bytes.fromhex(session_id)).hexdigest()[:32] != value.get("usb_serial")
    ):
        fail("current SSH session identity is invalid")
    try:
        network = ipaddress.IPv4Network(value.get("network"), strict=True)
    except (TypeError, ValueError):
        fail("current SSH session network is invalid")
    if network.prefixlen != 30 or not _is_rfc1918(network):
        fail("current SSH session network is not a private /30")
    host_address, phone_address = (str(address) for address in network.hosts())
    if value.get("host_address") != host_address or value.get("phone_address") != phone_address:
        fail("current SSH session addresses do not match its network")
    if (
        type(value.get("vendor_id")) is not int
        or not 0 <= value["vendor_id"] <= 0xFFFF
        or type(value.get("product_id")) is not int
        or not 0 <= value["product_id"] <= 0xFFFF
        or type(value.get("wait_seconds")) is not int
        or not 1 <= value["wait_seconds"] <= 3600
        or not isinstance(value.get("interface"), str)
        or re.fullmatch(r"[A-Za-z0-9_.-]{1,15}", value["interface"]) is None
    ):
        fail("current SSH session endpoint metadata is invalid")
    target_value = value.get("target")
    usb_serial = value.get("usb_serial")
    if not isinstance(target_value, str) or not isinstance(usb_serial, str):
        fail("current SSH session path identity is invalid")
    directory = _session_directory(_runtime_root(), target_value, usb_serial)
    try:
        metadata = directory.lstat()
        owner = json.loads(_regular_file_bytes(directory / "owner.json", "session owner record"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"current SSH session directory is invalid: {error}")
    if (
        not stat.S_ISDIR(metadata.st_mode)
        or stat.S_ISLNK(metadata.st_mode)
        or metadata.st_uid != os.getuid()
        or owner
        != {
            "kind": SESSION_OWNER_KIND,
            "target": target_value,
            "usb_serial": usb_serial,
        }
    ):
        fail("current SSH session directory identity is invalid")
    if Path(value.get("private_key", "")).parent != directory:
        fail("current SSH private key is outside its session directory")
    if Path(value.get("known_hosts", "")).parent != directory:
        fail("current SSH host-key file is outside its session directory")
    for field in ("private_key", "known_hosts"):
        path = Path(value[field])
        try:
            metadata = path.lstat()
        except OSError as error:
            fail(f"current SSH session file is unavailable: {error}")
        if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
            fail(f"current SSH session file is invalid: {path}")
        if metadata.st_uid != os.getuid() or stat.S_IMODE(metadata.st_mode) != 0o600:
            fail(f"current SSH session file is not private: {path}")
    return value


def load_current_session(target: str, identity: dict[str, str]) -> dict[str, Any]:
    """Load only the session that completed USB and private identity binding."""
    if TARGET_NAME.fullmatch(target) is None:
        fail(f"invalid target name: {target}")
    path = _runtime_root() / "current" / f"{target}.json"
    if path.is_symlink() or not path.is_file():
        fail(f"no ready SSH session for {target}; run ./fplinux run {target} first")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"current SSH session state is invalid: {error}")
    return _validate_session(value, target, identity)


def reacquire_bound_session(session: dict[str, Any]) -> dict[str, Any]:
    """Survive a physical replug without weakening USB, host-key or session binding."""
    session = _validate_session(session)
    return _wait_for_authenticated_endpoint(session, scan_host_key=False)


def open_shell(session: dict[str, Any]) -> None:
    """Replace the runner with the session-scoped interactive SSH client."""
    session = _validate_session(session)
    argv = _ssh_argv(session)
    argv[1:1] = ["-tt"]
    os.execv(argv[0], argv)


def run_remote(
    session: dict[str, Any],
    command: str,
    *,
    capture_output: bool = False,
) -> subprocess.CompletedProcess[str]:
    """Run one command through the already-bound SSH endpoint."""
    session = _validate_session(session)
    return subprocess.run(
        _ssh_argv(session, command),
        capture_output=capture_output,
        text=True,
        check=False,
    )


def _remote_path(value: str, name: str) -> str:
    if len(value) > 200 or REMOTE_PATH.fullmatch(value) is None:
        fail(f"{name} must be an absolute file path using only letters, digits, '.', '_', '-'")
    if any(component.strip(".") == "" for component in value.split("/")[1:]):
        fail(f"{name} must not contain dot-only path components")
    return value


def _sftp_quote(value: str) -> str:
    if any(character in value for character in ("\0", "\n", "\r")):
        fail("SFTP path contains a line separator")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _sftp(session: dict[str, Any], command: str) -> subprocess.CompletedProcess[str]:
    argv = [
        _require_tool("sftp"),
        "-q",
        "-b",
        "-",
        *_ssh_options(session),
        f"root@{session['phone_address']}",
    ]
    return subprocess.run(
        argv,
        input=command + "\n",
        capture_output=True,
        text=True,
        check=False,
    )


def _remote_metadata(session: dict[str, Any], remote: str) -> tuple[int, str]:
    command = (
        f"if [ -f '{remote}' ] && [ -r '{remote}' ]; then "
        f"set -- $(sha256sum '{remote}'); size=$(wc -c < '{remote}'); "
        'printf "%s %s\\n" "$size" "$1"; else exit 44; fi'
    )
    result = run_remote(session, command, capture_output=True)
    if result.returncode:
        fail(f"the phone cannot read {remote}")
    fields = result.stdout.strip().split()
    if len(fields) != 2 or not fields[0].isdigit() or SHA256.fullmatch(fields[1]) is None:
        fail("the phone returned invalid file metadata")
    return int(fields[0]), fields[1]


def upload(session: dict[str, Any], local_name: str, remote_name: str) -> None:
    """Upload through SFTP, then verify and atomically publish on the phone."""
    session = _validate_session(session)
    remote = _remote_path(remote_name, "upload destination")
    local = Path(local_name)
    try:
        metadata = local.lstat()
    except OSError as error:
        fail(f"cannot inspect upload source {local}: {error}")
    if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        fail(f"upload source is not a regular file: {local}")
    expected_hash = _sha256_file(local)
    expected_size = metadata.st_size
    directory = remote.rsplit("/", 1)[0] or "/"
    free_kib = expected_size // 1024 + 64
    check = run_remote(
        session,
        f"[ -d '{directory}' ] && free=$(df -Pk '{directory}' | "
        f"awk 'NR==2{{print $4}}') && [ \"$free\" -ge {free_kib} ]",
        capture_output=True,
    )
    if check.returncode:
        fail("upload destination is missing or does not have enough free space")
    nonce = os.getrandom(12).hex()
    temporary = f"{directory.rstrip('/')}/.fplinux-upload.{nonce}"
    if directory == "/":
        temporary = f"/.fplinux-upload.{nonce}"
    published = False
    try:
        result = _sftp(
            session,
            f"put {_sftp_quote(str(local))} {_sftp_quote(temporary)}",
        )
        if result.returncode:
            detail = result.stderr.strip().splitlines()
            fail("SFTP upload failed" + (f": {detail[-1]}" if detail else ""))
        if _sha256_file(local) != expected_hash or local.stat().st_size != expected_size:
            fail(f"upload source changed while it was sent: {local}")
        publish = run_remote(
            session,
            f"got=$(sha256sum '{temporary}'); got=${{got%% *}}; "
            f"size=$(wc -c < '{temporary}'); "
            f'[ "$got" = \'{expected_hash}\' ] && [ "$size" -eq {expected_size} ] && '
            f"[ ! -d '{remote}' ] && [ ! -L '{remote}' ] && mv -f '{temporary}' '{remote}'",
            capture_output=True,
        )
        if publish.returncode:
            fail("device-side SHA-256 verification or atomic upload publication failed")
        published = True
    finally:
        if not published:
            run_remote(session, f"rm -f '{temporary}'", capture_output=True)
    print(
        f"upload verified: {local} ({expected_size} bytes, sha256={expected_hash}) -> {remote}",
        flush=True,
    )


def pull(session: dict[str, Any], remote_name: str, local_name: str) -> None:
    """Download through SFTP and publish locally only after end-to-end verification."""
    session = _validate_session(session)
    remote = _remote_path(remote_name, "pull source")
    expected_size, expected_hash = _remote_metadata(session, remote)
    destination = Path(local_name)
    parent = destination.parent
    if parent.is_symlink() or not parent.is_dir():
        fail(f"pull destination directory is missing or invalid: {parent}")
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=parent,
            prefix=f".{destination.name}.",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
        temporary.chmod(0o600)
        result = _sftp(
            session,
            f"get {_sftp_quote(remote)} {_sftp_quote(str(temporary))}",
        )
        if result.returncode:
            detail = result.stderr.strip().splitlines()
            fail("SFTP pull failed" + (f": {detail[-1]}" if detail else ""))
        actual_size = temporary.stat().st_size
        actual_hash = _sha256_file(temporary)
        if actual_size != expected_size or actual_hash != expected_hash:
            fail("downloaded file does not match the size and SHA-256 reported by the phone")
        if _remote_metadata(session, remote) != (expected_size, expected_hash):
            fail("pull source changed while it was downloaded")
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    message = f"pull verified: {remote} ({expected_size} bytes, sha256={expected_hash})"
    print(f"{message} -> {destination}", flush=True)
