# SPDX-License-Identifier: GPL-2.0-only
"""Content-derived identity for one target kernel visible on a device."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Mapping

_LOCALVERSION_PREFIX = "-fplinux-"
_LOCALVERSION_DIGEST_LENGTH = 16
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_TARGET = re.compile(r"[a-z0-9][a-z0-9._-]*\Z")


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


def _require_rootfs(value: Mapping[str, object]) -> dict[str, int | str]:
    if set(value) != {"sha256", "size"}:
        message = "rootfs identity must contain exactly sha256 and size"
        raise DeviceStateError(message)
    digest = _require_digest(value.get("sha256"), "rootfs SHA-256")
    size = value.get("size")
    if type(size) is not int or size < 0:
        message = "rootfs size must be a non-negative integer"
        raise DeviceStateError(message)
    return {"sha256": digest, "size": size}


def _require_receipt(value: Mapping[str, object]) -> dict[str, str]:
    if set(value) != {"recipe", "sha256"}:
        message = "Buildroot receipt must contain exactly recipe and sha256"
        raise DeviceStateError(message)
    return {
        "recipe": _require_digest(value.get("recipe"), "Buildroot recipe"),
        "sha256": _require_digest(value.get("sha256"), "Buildroot receipt SHA-256"),
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
    rootfs: Mapping[str, object],
    buildroot_receipt: Mapping[str, object],
    arch: str,
    defconfig: Path,
    dtb: str,
) -> str:
    """Hash the exact pre-Kbuild device inputs, excluding workspace orchestration."""
    payload = {
        "target": _require_target(target),
        "linux_recipe": _require_digest(linux_recipe, "prepared Linux recipe"),
        "bootstrap_recipe": _require_digest(bootstrap_recipe, "bootstrap recipe"),
        "rootfs": _require_rootfs(rootfs),
        "buildroot_receipt": _require_receipt(buildroot_receipt),
        "kernel": {
            "arch": _require_target(arch),
            "defconfig": _defconfig_identity(Path(defconfig)),
            "dtb": _require_relative(dtb, "target DTB"),
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
