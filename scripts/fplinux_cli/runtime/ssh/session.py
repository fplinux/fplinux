# SPDX-License-Identifier: GPL-2.0-only
"""Private Linux SSH session state."""

from __future__ import annotations

import contextlib
import hashlib
import ipaddress
import json
import os
import re
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from .commands import require_tool, ssh_config
from .errors import fail
from .material import (
    SESSION_BYTES,
    allocate_network,
    copy_personalized_image,
    is_rfc1918,
    mac_pair,
    public_key_body,
    session_block,
    usb_config,
)

SESSION_OWNER_KIND = "fplinux-host-session-owner"
TARGET_NAME = re.compile(r"[a-z0-9][a-z0-9._-]*")
SHA256 = re.compile(r"[0-9a-f]{64}")


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


def _selected_runtime_root(path: Path) -> Path:
    """Use the caller's explicit private session directory."""
    if not path.is_absolute():
        fail("session runtime directory must be absolute")
    return _private_directory(path)


def _session_runtime_root(session: dict[str, Any]) -> Path:
    """Keep every operation bound to the directory chosen during preparation."""
    value = session.get("runtime_root")
    if not isinstance(value, str):
        fail("session runtime directory is invalid")
    return _selected_runtime_root(Path(value))


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


def _regular_file_bytes(path: Path, name: str) -> bytes:
    try:
        metadata = path.lstat()
        data = path.read_bytes()
    except OSError as error:
        fail(f"cannot read {name}: {error}")
    if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        fail(f"{name} is not a regular file: {path}")
    return data


def host_route_networks() -> set[ipaddress.IPv4Network]:
    result = subprocess.run(
        [require_tool("ip"), "-4", "-j", "route", "show", "table", "all"],
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
        except OSError, UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError:
            continue
        if network.prefixlen == 30 and is_rfc1918(network):
            networks.add(network)
    return networks


def _choose_network(sessions: Path) -> ipaddress.IPv4Network:
    return allocate_network(host_route_networks() | _active_session_networks(sessions))


def _session_directory(root: Path, target: str, usb_serial: str) -> Path:
    if TARGET_NAME.fullmatch(target) is None or re.fullmatch(r"[0-9a-f]{32}", usb_serial) is None:
        fail("session directory identity is invalid")
    identity = f"{target}\0{usb_serial}".encode("ascii")
    name = hashlib.sha256(identity).hexdigest()[:32]
    return root / "sessions" / name


def _remove_owned_session(directory: Path, target: str, usb_serial: str, *, root: Path) -> None:
    """Remove only a session directory carrying our exact private owner record."""
    expected = _session_directory(root, target, usb_serial)
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


def _retire_target_sessions(root: Path, target: str, *, current_serial: str) -> None:
    """Remove owned superseded sessions after the replacement is ready."""
    current = _session_directory(root, target, current_serial)
    sessions = _private_directory(root / "sessions")
    for directory in sessions.iterdir():
        if directory == current or directory.is_symlink() or not directory.is_dir():
            continue
        owner_path = directory / "owner.json"
        try:
            owner = json.loads(_regular_file_bytes(owner_path, "session owner record"))
        except SystemExit:
            continue
        except UnicodeDecodeError, json.JSONDecodeError:
            continue
        if (
            isinstance(owner, dict)
            and owner.get("kind") == SESSION_OWNER_KIND
            and owner.get("target") == target
            and isinstance(owner.get("usb_serial"), str)
            and directory == _session_directory(root, target, owner["usb_serial"])
        ):
            _remove_owned_session(directory, target, owner["usb_serial"], root=root)


def prepare_session(  # noqa: PLR0913 -- bundle identity and runtime destination are distinct.
    image: Path,
    personalization: dict[str, Any],
    target: str,
    linux_usb: dict[str, Any],
    *,
    runtime_root: Path,
    bundle_generation: str,
    prepared_data_sha256: str | None,
) -> dict[str, Any]:
    """Create keys, addressing and one mode-0600 personalized image copy."""
    if TARGET_NAME.fullmatch(target) is None:
        fail(f"invalid target name: {target}")
    root = _selected_runtime_root(runtime_root)
    for tool in ("ip", "ssh", "ssh-keygen", "ssh-keyscan"):
        require_tool(tool)
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
        host_mac, device_mac = mac_pair()
        keygen = require_tool("ssh-keygen")
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
        key_body = public_key_body(public_key)
        block = session_block(
            session_id,
            rng_seed,
            key_body,
            usb_config(usb_serial, network, host_mac, device_mac, transport="ncm"),
        )
        copy_personalized_image(image, personalized_image, personalization, block)
    except BaseException:
        _remove_owned_session(session_directory, target, usb_serial, root=root)
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
        "runtime_root": str(root),
        "bundle_generation": bundle_generation,
        "prepared_data_sha256": prepared_data_sha256,
    }
    try:
        _write_json(state_path, state)
    except BaseException:
        _remove_owned_session(session_directory, target, usb_serial, root=root)
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
    expected = (
        _session_directory(_session_runtime_root(session), target, usb_serial) / "ramboot.bin"
    )
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
    root = _session_runtime_root(session)
    directory = _session_directory(root, target, usb_serial)
    state_path = directory / "session.json"
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except OSError, UnicodeDecodeError, json.JSONDecodeError:
        state = None
    ready = False
    if isinstance(state, dict) and state.get("status") == "ready":
        try:
            validate_session(state)
        except SystemExit:
            pass
        else:
            ready = True
    if not ready:
        _remove_owned_session(directory, target, usb_serial, root=root)


def mark_current(session: dict[str, Any]) -> dict[str, Any]:
    session = dict(session)
    session["status"] = "ready"
    target = session.get("target")
    usb_serial = session.get("usb_serial")
    if not isinstance(target, str) or not isinstance(usb_serial, str):
        fail("cannot publish a session with invalid identity")
    directory = Path(session["private_key"]).parent
    root = _session_runtime_root(session)
    expected = _session_directory(root, target, usb_serial)
    if directory != expected:
        fail("cannot publish a session outside its runtime directory")
    previous = load_saved_session(target, runtime_root=root)
    current = _private_directory(root / "current")
    config_path = current / f"{target}.ssh-config"
    config_published = False
    try:
        _write_json(directory / "session.json", session)
        _write_private_text(config_path, ssh_config(session))
        config_published = True
        _write_json(current / f"{target}.json", session)
    except BaseException:
        if config_published:
            with contextlib.suppress(OSError):
                if previous is None:
                    config_path.unlink(missing_ok=True)
                else:
                    _write_private_text(config_path, ssh_config(previous))
        if previous is None or previous["usb_serial"] != usb_serial:
            _remove_owned_session(directory, target, usb_serial, root=root)
        raise
    _retire_target_sessions(root, target, current_serial=usb_serial)
    return session


def require_prepared_content(
    session: dict[str, Any], *, bundle_generation: str, prepared_data_sha256: str | None
) -> None:
    """Refuse to reconnect after the selected bundle or personal contents changed."""
    if session["bundle_generation"] != bundle_generation:
        fail("the running phone uses a different build; shut it down before starting this build")
    if session["prepared_data_sha256"] != prepared_data_sha256:
        fail(
            "the running phone uses different prepared phone data; "
            "shut it down before using this data"
        )


def validate_session(
    value: object,
    target: str | None = None,
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
        "runtime_root",
        "bundle_generation",
        "prepared_data_sha256",
    }
    if not isinstance(value, dict) or set(value) != required:
        fail("current SSH session state has unexpected fields")
    generation = value["bundle_generation"]
    prepared = value["prepared_data_sha256"]
    if (
        not isinstance(generation, str)
        or SHA256.fullmatch(generation) is None
        or (
            prepared is not None
            and (not isinstance(prepared, str) or SHA256.fullmatch(prepared) is None)
        )
    ):
        fail("current SSH session content identity is invalid")
    if value.get("status") != "ready":
        fail("current SSH session state is not ready")
    if target is not None and value.get("target") != target:
        fail("current SSH session belongs to another target")
    session_id = value.get("session_id")
    if (
        not isinstance(session_id, str)
        or SHA256.fullmatch(session_id) is None
        or hashlib.sha256(bytes.fromhex(session_id)).hexdigest()[:32] != value.get("usb_serial")
    ):
        fail("current SSH session identity is invalid")
    try:
        network = ipaddress.IPv4Network(value.get("network"), strict=True)
    except TypeError, ValueError:
        fail("current SSH session network is invalid")
    if network.prefixlen != 30 or not is_rfc1918(network):
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
    directory = _session_directory(_session_runtime_root(value), target_value, usb_serial)
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


def _read_current_session(path: Path, target: str, *, root: Path) -> dict[str, Any]:
    """Read and validate the saved binding without deciding whether absence is allowed."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"current SSH session state is invalid: {error}")
    session = validate_session(value, target)
    if _session_runtime_root(session) != root:
        fail("current SSH session belongs to another runtime directory")
    return session


def load_saved_session(target: str, *, runtime_root: Path) -> dict[str, Any] | None:
    """Load an optional validated ready session without probing USB presence."""
    if TARGET_NAME.fullmatch(target) is None:
        fail(f"invalid target name: {target}")
    root = _selected_runtime_root(runtime_root)
    path = root / "current" / f"{target}.json"
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return None
    except OSError as error:
        fail(f"current SSH session state is invalid: {error}")
    if not stat.S_ISREG(metadata.st_mode):
        fail("current SSH session state is not a regular file")
    return _read_current_session(path, target, root=root)
