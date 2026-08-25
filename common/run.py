#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run a validated FPLinux RAM-only bundle through its fixed platform adapter."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import signal
import sys
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any, NoReturn

if TYPE_CHECKING:
    from types import FrameType, ModuleType

ADAPTER_PATH = "runner/platform_adapter.py"
IDENTITY_PATH = "runner/identity.py"
SSH_HELPER_PATH = "runner/ssh_transport.py"
MINIMUM_PYTHON = (3, 11)
TRANSPORTS = frozenset({"usb-ncm", "none"})
_identity_module: ModuleType | None = None


def fail(message: str) -> NoReturn:
    """Stop before invoking the platform adapter."""
    raise SystemExit(f"RAM runner failed: {message}")


def identity_module() -> ModuleType:
    """Load the shared identity contract from a bundle or source checkout."""
    global _identity_module  # noqa: PLW0603

    if _identity_module is not None:
        return _identity_module
    candidates = (
        Path(__file__).with_name("identity.py"),
        Path(__file__).resolve().parents[1] / "scripts/fplinux_cli/identity.py",
    )
    source = next((path for path in candidates if path.is_file()), None)
    if source is None:
        fail("runner identity contract is missing")
    spec = importlib.util.spec_from_file_location("fplinux_runtime_identity", source)
    if spec is None or spec.loader is None:
        fail("runner identity contract cannot be loaded")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _identity_module = module
    return module


def validate_runtime_identity(runtime: dict[str, Any]) -> dict[str, Any]:
    """Normalize identity only after the caller verifies the helper bytes."""
    module = identity_module()
    if getattr(module, "RUNTIME_IDENTITY_PATH", None) != IDENTITY_PATH:
        fail("runner identity path differs from the shared contract")
    try:
        runtime["identity"] = module.validate_runtime_identity(runtime.get("identity"))
    except ValueError as error:
        fail(str(error))
    return runtime


def digest(path: Path) -> str:
    """Return a file SHA-256 digest."""
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def require_object(value: object, keys: set[str], name: str) -> dict[str, Any]:
    """Validate an exact-key JSON object."""
    if not isinstance(value, dict) or set(value) != keys:
        fail(f"{name} must contain exactly: {', '.join(sorted(keys))}")
    return value


def require_string(value: object, name: str) -> str:
    """Validate a non-empty string."""
    if not isinstance(value, str) or not value:
        fail(f"{name} must be a non-empty string")
    return value


def require_transport(value: object) -> str:
    """Validate the one host-side transport contract for this bundle."""
    transport = require_string(value, "runtime transport")
    if transport not in TRANSPORTS:
        fail("runtime transport must be one of: none, usb-ncm")
    return transport


def require_optional_profile(value: object) -> str | None:
    """Validate the selected profile identity without inventing a default."""
    if value is None:
        return None
    return require_string(value, "runtime profile")


def relative_name(value: object, name: str) -> str:
    """Validate a normalized bundle-relative path."""
    text = require_string(value, name)
    path = PurePosixPath(text)
    if path.is_absolute() or ".." in path.parts or path.as_posix() != text:
        fail(f"{name} must be a normalized relative path")
    return text


def require_integer(
    table: dict[str, Any],
    key: str,
    name: str,
    *,
    bounds: tuple[int, int],
    alignment: int = 1,
) -> int:
    """Validate a bounded integer field."""
    value = table.get(key)
    minimum, maximum = bounds
    if type(value) is not int or not minimum <= value <= maximum:
        fail(f"{name} must be an integer in {minimum}..{maximum}")
    if value % alignment:
        fail(f"{name} must be aligned to {alignment} bytes")
    return value


def validate_usb(value: object, name: str, *, interface_fields: bool = False) -> dict[str, Any]:
    """Validate USB identity and timeout metadata."""
    base_fields = {"vendor_id", "product_id", "wait_seconds"}
    fields = base_fields | {"keyboard_interface"}
    if interface_fields:
        device = require_object(value, fields, name)
    else:
        device = require_object(value, base_fields, name)
    result = {
        "vendor_id": require_integer(device, "vendor_id", f"{name} vendor_id", bounds=(0, 0xFFFF)),
        "product_id": require_integer(
            device,
            "product_id",
            f"{name} product_id",
            bounds=(0, 0xFFFF),
        ),
        "wait_seconds": require_integer(
            device,
            "wait_seconds",
            f"{name} wait_seconds",
            bounds=(1, 3600),
        ),
    }
    if interface_fields:
        result["keyboard_interface"] = require_integer(
            device,
            "keyboard_interface",
            f"{name} keyboard_interface",
            bounds=(0, 255),
        )
    return result


def validate_path_table(value: object, name: str) -> dict[str, str]:
    """Validate a non-empty role-to-path table."""
    if not isinstance(value, dict) or not value:
        fail(f"{name} must be a non-empty object")
    result: dict[str, str] = {}
    for role, path in value.items():
        if not isinstance(role, str) or not role:
            fail(f"{name} role must be a non-empty string")
        result[role] = relative_name(path, f"{name} {role}")
    if len(set(result.values())) != len(result):
        fail(f"{name} paths must be unique")
    return result


def load_runtime_manifest(path: Path) -> dict[str, Any]:
    """Load and validate the generic runtime contract."""
    require_file(path)
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"runtime manifest is invalid: {error}")
    root = require_object(
        document,
        {
            "target",
            "profile",
            "identity",
            "transport",
            "image",
            "personalization",
            "addresses",
            "usb",
            "assets",
            "adapter",
            "host_tools",
            "sha256",
        },
        "runtime manifest",
    )
    root["target"] = require_string(root.get("target"), "runtime target")
    root["profile"] = require_optional_profile(root.get("profile"))
    if not isinstance(root.get("identity"), dict):
        fail("runtime identity must be an object")
    root["transport"] = require_transport(root.get("transport"))
    root["image"] = relative_name(root.get("image"), "runtime image")

    descriptor = require_object(
        root.get("personalization"),
        {"offset", "bytes", "template_sha256"},
        "runtime personalization",
    )
    offset = require_integer(
        descriptor,
        "offset",
        "runtime personalization offset",
        bounds=(512, 0xFFFFFFFF),
        alignment=64,
    )
    size = require_integer(
        descriptor,
        "bytes",
        "runtime personalization bytes",
        bounds=(512, 512),
    )
    template_hash = descriptor.get("template_sha256")
    if (
        not isinstance(template_hash, str)
        or len(template_hash) != 64
        or any(character not in "0123456789abcdef" for character in template_hash)
    ):
        fail("runtime personalization template_sha256 must be a lowercase SHA-256 digest")
    root["personalization"] = {
        "offset": offset,
        "bytes": size,
        "template_sha256": template_hash,
    }

    addresses = require_object(root.get("addresses"), {"fdl1", "payload"}, "addresses")
    root["addresses"] = {
        "fdl1": require_integer(
            addresses,
            "fdl1",
            "FDL1 address",
            bounds=(0, 0xFFFFFFFF),
            alignment=4,
        ),
        "payload": require_integer(
            addresses,
            "payload",
            "payload address",
            bounds=(0, 0xFFFFFFFF),
            alignment=4,
        ),
    }
    usb = require_object(root.get("usb"), {"bootrom", "linux_gadget"}, "USB metadata")
    root["usb"] = {
        "bootrom": validate_usb(usb.get("bootrom"), "bootrom USB"),
        "linux_gadget": validate_usb(
            usb.get("linux_gadget"), "linux_gadget USB", interface_fields=True
        ),
    }
    root["assets"] = validate_path_table(root.get("assets"), "runtime assets")
    root["host_tools"] = validate_path_table(root.get("host_tools"), "runtime host tools")
    if not isinstance(root.get("adapter"), dict):
        fail("runtime adapter must be an object")

    expected_paths = {
        root["image"],
        ADAPTER_PATH,
        IDENTITY_PATH,
        *root["assets"].values(),
        *root["host_tools"].values(),
        SSH_HELPER_PATH,
    }
    hashes = require_object(root.get("sha256"), expected_paths, "runtime hashes")
    for relative, value in hashes.items():
        if (
            not isinstance(value, str)
            or len(value) != 64
            or any(character not in "0123456789abcdef" for character in value)
        ):
            fail(f"runtime hash for {relative} must be a lowercase SHA-256 digest")
    return root


def require_file(path: Path, *, executable: bool = False) -> None:
    """Require a regular, non-symlink file and optionally execute access."""
    if path.is_symlink() or not path.is_file():
        fail(f"required file is missing or invalid: {path}")
    if executable and not os.access(path, os.X_OK):
        fail(f"host tool is not executable: {path}")


def host_preflight() -> None:
    """Reject an unsupported host runtime before any phone operation."""
    if sys.version_info < MINIMUM_PYTHON:
        version = f"{sys.version_info.major}.{sys.version_info.minor}"
        fail(f"Python 3.11 or newer is required (found {version})")


def load_module(path: Path, name: str) -> ModuleType:
    """Load one already-hash-verified module from its fixed bundle path."""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail(f"{name} could not be loaded")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def load_adapter(path: Path) -> ModuleType:
    """Load only the adapter bundled at the fixed platform-adapter path."""
    module = load_module(path, "fplinux_platform_adapter")
    if not callable(getattr(module, "run", None)):
        fail("platform adapter does not expose run(bundle, runtime, session)")
    return module


def arguments() -> argparse.Namespace:
    """Parse either a fresh RAM load or an authenticated reconnect action."""
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--reconnect",
        action="store_true",
        help="reconnect to the ready SSH session for this exact bundle",
    )
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--exec", dest="exec_command", metavar="COMMAND")
    action.add_argument("--upload", nargs=2, metavar=("LOCAL", "REMOTE"))
    action.add_argument("--pull", nargs=2, metavar=("REMOTE", "LOCAL"))
    result = parser.parse_args()
    if not result.reconnect and any(
        value is not None for value in (result.exec_command, result.upload, result.pull)
    ):
        parser.error("--exec, --upload and --pull require --reconnect")
    return result


def main() -> None:
    """Verify the bundle closure and enter the fixed adapter."""
    options = arguments()
    bundle = Path(__file__).resolve().parent.parent
    runtime = load_runtime_manifest(bundle / "runtime-manifest.json")
    for relative, expected in runtime["sha256"].items():
        path = bundle / relative
        require_file(path, executable=relative in runtime["host_tools"].values())
        actual = digest(path)
        if actual != expected:
            fail(f"{relative} SHA256 mismatch: expected {expected}, got {actual}")
    runtime = validate_runtime_identity(runtime)
    image = bundle / runtime["image"]
    if image.read_bytes()[:4] != b"DHTB":
        fail("RAM payload does not have a DHTB header")
    host_preflight()
    if options.reconnect:
        if runtime["transport"] != "usb-ncm":
            fail("--reconnect is unavailable when runtime transport is none")
        ssh = load_module(bundle / SSH_HELPER_PATH, "ssh_transport")
        identity = ssh.bundle_identity(bundle, runtime)
        session = ssh.load_current_session(runtime["target"], identity)
        session = ssh.reacquire_bound_session(session)
        if options.exec_command is not None:
            result = ssh.run_remote(session, options.exec_command)
            if result.returncode:
                raise SystemExit(result.returncode)
            return
        if options.upload is not None:
            ssh.upload(session, options.upload[0], options.upload[1])
            return
        if options.pull is not None:
            ssh.pull(session, options.pull[0], options.pull[1])
            return
        ssh.open_shell(session)
        fail("SSH client returned without replacing the runner")

    adapter = load_adapter(bundle / ADAPTER_PATH)
    session = None
    previous_handlers: dict[signal.Signals, Any] = {}

    def stop_session(signum: int, _frame: FrameType | None) -> None:
        raise SystemExit(128 + signum)

    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        previous_handlers[signum] = signal.signal(signum, stop_session)
    try:
        ssh = load_module(bundle / SSH_HELPER_PATH, "ssh_transport")
        identity = ssh.bundle_identity(bundle, runtime)
        session = ssh.prepare_session(
            image,
            runtime["personalization"],
            runtime["target"],
            runtime["usb"]["linux_gadget"],
            identity,
        )
        adapter.run(bundle, runtime, session)
    finally:
        if session is not None:
            ssh.finish_session(session)
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)


if __name__ == "__main__":
    main()
