# SPDX-License-Identifier: GPL-2.0-only
"""Admit one immutable bundle and its selected runtime closure."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any, NoReturn

if TYPE_CHECKING:
    from types import ModuleType

    from .windows_host import WindowsBundle

IDENTITY_PATH = "scripts/fplinux_cli/manifests/identity.py"
SSH_HELPER_PATH = "scripts/fplinux_cli/runtime/ssh_transport.py"
EVENTS_HELPER_PATH = "scripts/fplinux_cli/runtime/events.py"
REQUIRED_PYTHON = (3, 14)


@dataclass(frozen=True)
class ValidatedBundle:
    """One admitted runtime closure and the identities reused by its host operations."""

    root: Path
    runtime: dict[str, Any]
    generation: str
    device_identity: str
    windows: WindowsBundle | None


def fail(message: str) -> NoReturn:
    """Stop before invoking the platform adapter."""
    raise SystemExit(f"fplinux run: {message}")


def identity_module() -> ModuleType:
    """Use the canonical identity implementation in the delivered closure."""
    from fplinux_cli.manifests import identity  # noqa: PLC0415

    return identity


def adapter_path(runtime: dict[str, Any]) -> str:
    """Select the adapter from the admitted platform identity."""
    identity = runtime.get("identity")
    platform_identity = identity.get("platform") if isinstance(identity, dict) else None
    platform = platform_identity.get("name") if isinstance(platform_identity, dict) else None
    if not isinstance(platform, str) or re.fullmatch(r"[a-z0-9][a-z0-9-]*", platform) is None:
        fail("runtime platform name must be a normalized basename")
    return f"platforms/{platform}/host/adapter.py"


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
    try:
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()
    except OSError as error:
        fail(str(error))


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
    if transport != "usb-ncm":
        fail("runtime transport must be usb-ncm")
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
            "build_type",
            "release_id",
            "source_commit",
            "source_snapshot",
            "identity",
            "transport",
            "image",
            "personalization",
            "addresses",
            "usb",
            "assets",
            "adapter",
            "host_tools",
            "nand",
            "device_data",
            "sha256",
        },
        "runtime manifest",
    )
    root["target"] = require_string(root.get("target"), "runtime target")
    root["profile"] = require_optional_profile(root.get("profile"))
    if root.get("build_type") not in ("release", "debug"):
        fail("runtime build type must be release or debug")
    root["release_id"] = require_string(root["release_id"], "runtime release ID")
    if root["source_commit"] is not None:
        root["source_commit"] = require_string(root["source_commit"], "runtime source commit")
    root["source_snapshot"] = require_string(root["source_snapshot"], "runtime source snapshot")
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
    selected_adapter = adapter_path(root)
    root["nand"] = validate_nand(root["nand"], platform=root["identity"]["platform"]["name"])

    root["device_data"] = validate_device_data(root["device_data"])
    if root["device_data"] is not None and root["nand"] is None:
        fail("runtime preparation requires a declared NAND reader")
    expected_paths = {
        root["image"],
        "fplinux.py",
        selected_adapter,
        IDENTITY_PATH,
        *root["assets"].values(),
        *root["host_tools"].values(),
        SSH_HELPER_PATH,
        EVENTS_HELPER_PATH,
        *(
            f"scripts/fplinux_cli/runtime/{name}.py"
            for name in ("cli", "workflow", "bundle", "boot")
        ),
    }
    if root["device_data"] is not None:
        for field in ("parser", "ecc_module"):
            if root["device_data"][field] is not None:
                expected_paths.add(root["device_data"][field])
    hashes = root.get("sha256")
    if not isinstance(hashes, dict) or not expected_paths.issubset(hashes):
        fail("runtime hashes do not include the required runtime closure")
    for relative, value in hashes.items():
        relative_name(relative, "runtime hash path")
        if (
            not isinstance(value, str)
            or len(value) != 64
            or any(character not in "0123456789abcdef" for character in value)
        ):
            fail(f"runtime hash for {relative} must be a lowercase SHA-256 digest")
    return root


def validate_device_data(value: object) -> dict[str, Any] | None:
    """Admit the exact preparation inputs delivered for this bundle."""
    if value is None:
        return None
    from fplinux_cli.manifests.audio import validate_audio_policy  # noqa: PLC0415

    descriptor = require_object(
        value, {"parser", "groups", "audio_policy", "ecc_module"}, "device data"
    )
    for field in ("parser", "ecc_module"):
        if descriptor[field] is not None:
            descriptor[field] = relative_name(descriptor[field], f"device data {field}")
    descriptor["audio_policy"] = validate_audio_policy(descriptor["audio_policy"])
    if not isinstance(descriptor["groups"], dict) or not descriptor["groups"]:
        fail("device data groups must be a non-empty object")
    for group, entries in descriptor["groups"].items():
        if not isinstance(group, str) or not group or not isinstance(entries, list):
            fail("device data groups must contain named file lists")
        for entry in entries:
            if not isinstance(entry, dict) or not {"source", "destination"}.issubset(entry):
                fail(f"device data group {group} contains an invalid file")
            relative_name(entry["source"], f"device data {group} source")
            relative_name(entry["destination"], f"device data {group} destination")
    return descriptor


def validate_nand(value: object, *, platform: str) -> dict[str, Any] | None:
    """Admit the read-only reader contract even when no preparation is required."""
    if value is None:
        return None
    fields = {"raw_device", "id", "raw_page_bytes"}
    if not isinstance(value, dict) or set(value) not in (
        {"raw_device"},
        fields,
        fields | {"ecc_format"},
    ):
        fail("runtime NAND reader requires raw_device and optionally id with raw_page_bytes")
    if platform != "ums9117":
        fail("this platform does not support the runtime NAND reader")
    if (
        not isinstance(value["raw_device"], str)
        or re.fullmatch(r"/dev/[A-Za-z0-9][A-Za-z0-9._-]*", value["raw_device"]) is None
    ):
        fail("runtime NAND raw_device must name one device directly under /dev")
    if "id" in value:
        require_integer(value, "id", "runtime NAND id", bounds=(0, 0xFFFF))
        require_integer(
            value, "raw_page_bytes", "runtime NAND raw_page_bytes", bounds=(1, 0xFFFFFFFF)
        )
    if "ecc_format" in value and (
        value["ecc_format"] != "ums9117-bch12" or value["raw_page_bytes"] != 2176
    ):
        fail("runtime NAND ECC requires ums9117-bch12 with 2176-byte pages")
    return value


def admit_bundle(bundle: Path) -> ValidatedBundle:
    """Reject incomplete or changed content before host setup or USB discovery."""
    windows_archive = (bundle / "windows-host.json").is_file()
    if windows_archive != (os.name == "nt"):
        command = "Run.cmd on Windows" if windows_archive else "Run.sh on Linux"
        fail(f"Open this archive with {command}")
    runtime = load_runtime_manifest(bundle / "runtime-manifest.json")
    if not windows_archive and "60-fplinux.rules" not in runtime["sha256"]:
        fail("runtime hashes do not include the phone USB access rules")
    for relative, expected in runtime["sha256"].items():
        path = bundle / relative
        require_file(
            path, executable=not windows_archive and relative in runtime["host_tools"].values()
        )
        # The immutable build identity admits the image once, including its size.
        if relative == runtime["image"]:
            continue
        actual = digest(path)
        if actual != expected:
            fail(f"{relative} SHA256 mismatch: expected {expected}, got {actual}")
    runtime = validate_runtime_identity(runtime)
    from . import ssh_transport  # noqa: PLC0415

    identity = ssh_transport.bundle_identity(bundle, runtime)
    device_identity = ssh_transport.build_manifest_device_identity(bundle)
    windows = None
    if windows_archive:
        from .windows_host import validate_archive  # noqa: PLC0415

        windows = validate_archive(
            bundle,
            runtime,
            generation=identity["bundle_generation"],
            device_identity=device_identity,
        )
    return ValidatedBundle(
        bundle, runtime, identity["bundle_generation"], device_identity, windows
    )


def require_file(path: Path, *, executable: bool = False) -> None:
    """Require a regular, non-symlink file and optionally execute access."""
    if path.is_symlink() or not path.is_file():
        fail(f"required file is missing or invalid: {path}")
    if executable and not os.access(path, os.X_OK):
        fail(f"host tool is not executable: {path}")


def host_preflight() -> None:
    """Reject an unsupported host runtime before any phone operation."""
    version_info = (sys.version_info.major, sys.version_info.minor)
    if version_info != REQUIRED_PYTHON:
        version = f"{version_info[0]}.{version_info[1]}"
        fail(f"Python 3.14 is required (found {version})")


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
