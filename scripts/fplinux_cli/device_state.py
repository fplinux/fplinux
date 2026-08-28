# SPDX-License-Identifier: GPL-2.0-only
"""Content-derived identity for one target kernel visible on a device."""

from __future__ import annotations

import hashlib
import json
import re
from collections.abc import Mapping
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Sequence

_LOCALVERSION_PREFIX = "-fplinux-"
_LOCALVERSION_DIGEST_LENGTH = 16
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_TARGET = re.compile(r"[a-z0-9][a-z0-9._-]*\Z")
_KCONFIG_SYMBOL = re.compile(r"[A-Z0-9_]+\Z")


class DeviceStateError(ValueError):
    """Reject an incomplete or unsafe device/kernel identity input."""


def _require_digest(value: object, field: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise DeviceStateError(f"{field} must be a lowercase SHA-256 digest")
    return value


def _require_target(value: object) -> str:
    if not isinstance(value, str) or _TARGET.fullmatch(value) is None:
        message = "target must be a normalized target name"
        raise DeviceStateError(message)
    return value


def _require_relative(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise DeviceStateError(f"{field} must be a non-empty relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or path.as_posix() != value:
        raise DeviceStateError(f"{field} must be a normalized relative path: {value!r}")
    return value


def _require_profile(value: object) -> str | None:
    """Validate the optional named build profile without inventing a default."""
    if value is None:
        return None
    if not isinstance(value, str) or _TARGET.fullmatch(value) is None:
        message = "profile must be None or a normalized profile name"
        raise DeviceStateError(message)
    return value


def _require_kconfig_symbols(value: Sequence[object], field: str) -> list[str]:
    """Return exactly the Kconfig actions whose order Kbuild will consume."""
    result: list[str] = []
    for symbol in value:
        if not isinstance(symbol, str) or _KCONFIG_SYMBOL.fullmatch(symbol) is None:
            raise DeviceStateError(f"{field} contains an invalid Kconfig symbol")
        result.append(symbol)
    if len(result) != len(set(result)):
        raise DeviceStateError(f"{field} contains duplicate Kconfig symbols")
    return result


def _require_initramfs(value: Mapping[str, object]) -> dict[str, int | str]:
    if set(value) != {"sha256", "size"}:
        message = "initramfs identity must contain exactly sha256 and size"
        raise DeviceStateError(message)
    digest = _require_digest(value.get("sha256"), "initramfs SHA-256")
    size = value.get("size")
    if type(size) is not int or size < 0:
        message = "initramfs size must be a non-negative integer"
        raise DeviceStateError(message)
    return {"sha256": digest, "size": size}


def _require_receipt(value: Mapping[str, object]) -> dict[str, str]:
    if set(value) != {"recipe", "sha256"}:
        message = "initramfs receipt must contain exactly recipe and sha256"
        raise DeviceStateError(message)
    return {
        "recipe": _require_digest(value.get("recipe"), "initramfs recipe"),
        "sha256": _require_digest(value.get("sha256"), "initramfs receipt SHA-256"),
    }


def _require_root(value: Mapping[str, object]) -> dict[str, object]:
    """Normalize only root inputs that can alter the kernel or compiled DTB."""
    kind = value.get("kind")
    if kind == "initramfs":
        if set(value) != {"kind", "artifact", "receipt"}:
            message = "initramfs root must contain exactly kind, artifact and receipt"
            raise DeviceStateError(message)
        artifact = value.get("artifact")
        receipt = value.get("receipt")
        if not isinstance(artifact, Mapping) or not isinstance(receipt, Mapping):
            message = "initramfs root artifact and receipt must be tables"
            raise DeviceStateError(message)
        return {
            "kind": "initramfs",
            "artifact": _require_initramfs(artifact),
            "receipt": _require_receipt(receipt),
        }
    if kind != "external" or set(value) != {
        "kind",
        "filesystem",
        "partuuid",
        "wait_seconds",
    }:
        message = "external root contract is invalid"
        raise DeviceStateError(message)
    filesystem = value.get("filesystem")
    partuuid = value.get("partuuid")
    wait_seconds = value.get("wait_seconds")
    if filesystem != "ext4":
        message = "external root filesystem must be ext4"
        raise DeviceStateError(message)
    if not isinstance(partuuid, str) or re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{2}", partuuid) is None:
        message = "external root PARTUUID is invalid"
        raise DeviceStateError(message)
    if type(wait_seconds) is not int or not 1 <= wait_seconds <= 60:
        message = "external root wait_seconds must be in 1..60"
        raise DeviceStateError(message)
    return {
        "kind": "external",
        "filesystem": filesystem,
        "partuuid": partuuid,
        "wait_seconds": wait_seconds,
    }


def _defconfig_identity(path: Path) -> dict[str, str]:
    """Identify the defconfig by the bytes consumed by the builder."""
    try:
        content = path.read_bytes()
    except OSError as error:
        raise DeviceStateError(f"kernel defconfig is missing or invalid: {path}") from error
    return {"sha256": hashlib.sha256(content).hexdigest()}


def device_kernel_identity(  # noqa: PLR0913
    *,
    target: str,
    linux_recipe: str,
    bootstrap_recipe: str,
    root: Mapping[str, object],
    kbuild_implementation: str,
    arch: str,
    defconfig: Path,
    dtb: str,
    profile: str | None = None,
    config_enable: Sequence[object] = (),
    config_disable: Sequence[object] = (),
) -> str:
    """Hash the exact pre-Kbuild device inputs, excluding workspace orchestration."""
    payload = {
        "target": _require_target(target),
        "linux_recipe": _require_digest(linux_recipe, "prepared Linux recipe"),
        "bootstrap_recipe": _require_digest(bootstrap_recipe, "bootstrap recipe"),
        "root": _require_root(root),
        "kbuild_implementation": _require_digest(kbuild_implementation, "Kbuild implementation"),
        "kernel": {
            "arch": _require_target(arch),
            "defconfig": _defconfig_identity(Path(defconfig)),
            "dtb": _require_relative(dtb, "target DTB"),
            "profile": _require_profile(profile),
            "config_enable": _require_kconfig_symbols(config_enable, "config_enable"),
            "config_disable": _require_kconfig_symbols(config_disable, "config_disable"),
        },
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def localversion(identity: str) -> str:
    """Format one validated device/kernel identity for ``CONFIG_LOCALVERSION``."""
    return (
        _LOCALVERSION_PREFIX
        + _require_digest(identity, "device/kernel identity")[:_LOCALVERSION_DIGEST_LENGTH]
    )
