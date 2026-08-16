# SPDX-License-Identifier: GPL-2.0-only
"""Content-addressed shared cross-toolchain recipes and receipts."""

from __future__ import annotations

import hashlib
import json
import tempfile
from pathlib import Path
from typing import Any

from .common import fail, sha256_file

RECEIPT_NAME = ".fplinux-toolchain-receipt.json"


def _is_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def toolchain_defconfig(root: Path, platform: dict[str, Any]) -> Path:
    """Return the platform's declared toolchain-build defconfig."""
    relative = platform["buildroot"].get("toolchain_defconfig")
    if not isinstance(relative, str) or not relative:
        fail("platform Buildroot toolchain_defconfig is not declared")
    path = root / relative
    if not path.is_file():
        fail(f"platform toolchain defconfig is missing: {relative}")
    return path


def toolchain_external_defconfig(root: Path, platform: dict[str, Any]) -> Path:
    """Return the platform's declared external-toolchain consumer fragment."""
    relative = platform["buildroot"].get("toolchain_external_defconfig")
    if not isinstance(relative, str) or not relative:
        fail("platform Buildroot toolchain_external_defconfig is not declared")
    path = root / relative
    if not path.is_file():
        fail(f"platform toolchain external defconfig is missing: {relative}")
    return path


def _patches_entries(root: Path, platform: dict[str, Any]) -> list[dict[str, object]]:
    external = platform["buildroot"].get("external")
    if not isinstance(external, str) or not external:
        fail("platform Buildroot external tree is not declared")
    patches = root / external / "patches"
    if not patches.is_dir():
        return []
    entries: list[dict[str, object]] = []
    for child in sorted(patches.rglob("*")):
        relative = child.relative_to(patches).as_posix()
        if child.is_file():
            entries.append({"path": relative, "sha256": sha256_file(child)})
        elif child.is_dir():
            entries.append({"path": relative, "type": "directory"})
    return entries


def toolchain_recipe(
    root: Path,
    platform: dict[str, Any],
    container_lock: dict[str, Any],
    container_image_recipe: str,
) -> str:
    """Hash every input that shapes the shared cross toolchain."""
    if not _is_sha256(container_image_recipe):
        fail("container image recipe must be a lowercase SHA-256 digest")
    buildroot = container_lock.get("buildroot")
    if not isinstance(buildroot, dict):
        fail("container lock is missing Buildroot identity")
    version = buildroot.get("version")
    if not isinstance(version, str) or not version:
        fail("Buildroot version must be a non-empty string")
    source_sha256 = buildroot.get("sha256")
    if not _is_sha256(source_sha256):
        fail("Buildroot source must be a lowercase SHA-256 digest")
    defconfig = toolchain_defconfig(root, platform)
    manifest = {
        "container_image_recipe": container_image_recipe,
        "buildroot": {"version": version, "sha256": source_sha256},
        "defconfig": sha256_file(defconfig),
        "patches": _patches_entries(root, platform),
    }
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def toolchain_outputs(platform: dict[str, Any]) -> tuple[str, ...]:
    """Return the files a reusable toolchain tree must provide."""
    cross_compile = platform["linux"]["cross_compile"]
    if not isinstance(cross_compile, str) or not cross_compile:
        fail("platform cross compiler prefix is invalid")
    return (f"host/bin/{cross_compile}gcc",)


def receipt_matches(toolchain: Path, recipe: str, outputs: tuple[str, ...]) -> bool:
    """Return whether this toolchain tree is exactly the requested one."""
    try:
        raw: object = json.loads((toolchain / RECEIPT_NAME).read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return False
    if not isinstance(raw, dict) or set(raw) != {"recipe", "outputs"}:
        return False
    if raw.get("recipe") != recipe or raw.get("outputs") != list(outputs):
        return False
    return all((toolchain / relative).is_file() for relative in outputs)


def discard_success_receipt(toolchain: Path) -> None:
    """Remove a prior success receipt before rebuilding the toolchain."""
    (toolchain / RECEIPT_NAME).unlink(missing_ok=True)


def write_receipt(toolchain: Path, recipe: str, outputs: tuple[str, ...]) -> None:
    """Atomically publish a receipt after the toolchain outputs exist."""
    if not _is_sha256(recipe):
        fail("toolchain recipe must be a lowercase SHA-256 digest")
    if not toolchain.is_dir():
        fail(f"toolchain directory is invalid: {toolchain}")
    for relative in outputs:
        if not (toolchain / relative).is_file():
            fail(f"toolchain output is missing: {relative}")
    encoded = (
        json.dumps({"recipe": recipe, "outputs": list(outputs)}, sort_keys=True) + "\n"
    ).encode()
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=toolchain, prefix=f".{RECEIPT_NAME}.", delete=False
        ) as stream:
            temporary = Path(stream.name)
            stream.write(encoded)
        temporary.replace(toolchain / RECEIPT_NAME)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
