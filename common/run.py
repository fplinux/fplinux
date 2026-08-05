#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run a validated FPLinux RAM-only bundle through its fixed platform adapter."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any, NoReturn

if TYPE_CHECKING:
    from types import ModuleType

RUNTIME_MANIFEST_SCHEMA = "fplinux.runtime/v1"
ADAPTER_PATH = "runner/platform_adapter.py"
MINIMUM_PYTHON = (3, 11)


def fail(message: str) -> NoReturn:
    """Stop before invoking the platform adapter."""
    raise SystemExit(f"RAM runner failed: {message}")


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


def validate_usb(value: object, name: str) -> dict[str, Any]:
    """Validate USB identity and timeout metadata."""
    device = require_object(value, {"vendor_id", "product_id", "wait_seconds"}, name)
    return {
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
            "schema",
            "target",
            "display_name",
            "platform",
            "capability",
            "image",
            "addresses",
            "usb",
            "assets",
            "adapter",
            "host_tools",
            "sha256",
        },
        "runtime manifest",
    )
    if root.get("schema") != RUNTIME_MANIFEST_SCHEMA:
        fail(f"runtime manifest schema must be {RUNTIME_MANIFEST_SCHEMA}")
    for key in ("target", "display_name", "platform", "capability"):
        root[key] = require_string(root.get(key), f"runtime {key}")
    root["image"] = relative_name(root.get("image"), "runtime image")

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
    usb = require_object(root.get("usb"), {"bootrom", "linux_console"}, "USB metadata")
    root["usb"] = {
        name: validate_usb(usb.get(name), f"{name} USB") for name in ("bootrom", "linux_console")
    }
    root["assets"] = validate_path_table(root.get("assets"), "runtime assets")
    root["host_tools"] = validate_path_table(root.get("host_tools"), "runtime host tools")
    if not isinstance(root.get("adapter"), dict):
        fail("runtime adapter must be an object")

    expected_paths = {
        root["image"],
        ADAPTER_PATH,
        *root["assets"].values(),
        *root["host_tools"].values(),
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


def host_preflight(bundle: Path, runtime: dict[str, Any]) -> None:
    """Reject missing host runtime dependencies before any phone operation."""
    if sys.version_info < MINIMUM_PYTHON:
        version = f"{sys.version_info.major}.{sys.version_info.minor}"
        fail(f"Python 3.11 or newer is required (found {version})")

    ldd = shutil.which("ldd")
    if ldd is None:
        fail("glibc ldd is required to validate the bundled host tools")
    for role, relative in runtime["host_tools"].items():
        result = subprocess.run(
            [ldd, str(bundle / relative)],
            capture_output=True,
            text=True,
            check=False,
        )
        lines = [
            line.strip()
            for line in (result.stdout + "\n" + result.stderr).splitlines()
            if line.strip()
        ]
        missing = sorted(
            line.split("=>", 1)[0].strip() for line in lines if "=> not found" in line
        )
        if missing:
            fail(f"host tool {role} is missing shared libraries: {', '.join(missing)}")
        if result.returncode:
            detail = "; ".join(lines) or f"ldd exited with status {result.returncode}"
            fail(f"host tool {role} cannot run on this host: {detail}")


def load_adapter(path: Path) -> ModuleType:
    """Load only the adapter bundled at the fixed platform-adapter path."""
    spec = importlib.util.spec_from_file_location("fplinux_platform_adapter", path)
    if spec is None or spec.loader is None:
        fail("platform adapter could not be loaded")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if not callable(getattr(module, "run", None)):
        fail("platform adapter does not expose run(bundle, runtime)")
    return module


def main() -> None:
    """Verify the bundle closure and enter the fixed adapter."""
    bundle = Path(__file__).resolve().parent.parent
    runtime = load_runtime_manifest(bundle / "runtime-manifest.json")
    for relative, expected in runtime["sha256"].items():
        path = bundle / relative
        require_file(path, executable=relative in runtime["host_tools"].values())
        actual = digest(path)
        if actual != expected:
            fail(f"{relative} SHA256 mismatch: expected {expected}, got {actual}")
    image = bundle / runtime["image"]
    if image.read_bytes()[:4] != b"DHTB":
        fail("RAM payload does not have a DHTB header")
    host_preflight(bundle, runtime)
    adapter = load_adapter(bundle / ADAPTER_PATH)
    adapter.run(bundle, runtime)


if __name__ == "__main__":
    main()
