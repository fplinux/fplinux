# SPDX-License-Identifier: GPL-2.0-only
"""Validate and publish successful Alpine rootfs output receipts."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING

from fplinux_cli.common import fail, replace_file_atomically, sha256_file

from .lock import _is_sha256, _sha256

if TYPE_CHECKING:
    from pathlib import Path

RECEIPT_NAME = ".fplinux-rootfs-receipt.json"
ROOTFS_NAME = "rootfs.cpio"
INITRAMFS_NAME = "initramfs.cpio"


def rootfs_output(cache: Path, recipe: str) -> Path:
    """Return the immutable cache directory for one exact rootfs recipe."""
    _sha256(recipe, "Alpine rootfs recipe")
    return cache / "rootfs" / recipe


def _rootfs_record(path: Path) -> dict[str, int | str]:
    if path.is_symlink() or not path.is_file():
        fail(f"rootfs output is missing or invalid: {path}")
    return {"sha256": sha256_file(path), "size": path.stat().st_size}


def _receipt_data(output: Path, recipe: str) -> dict[str, object]:
    return {
        "recipe": _sha256(recipe, "Alpine rootfs recipe"),
        "rootfs": _rootfs_record(output / ROOTFS_NAME),
        "initramfs": (
            _rootfs_record(output / INITRAMFS_NAME) if (output / INITRAMFS_NAME).exists() else None
        ),
    }


def _read_receipt(output: Path) -> dict[str, object] | None:
    try:
        raw = json.loads((output / RECEIPT_NAME).read_text(encoding="utf-8"))
    except OSError, UnicodeDecodeError, json.JSONDecodeError:
        return None
    if not isinstance(raw, dict) or set(raw) != {"recipe", "rootfs", "initramfs"}:
        return None
    if not _is_sha256(raw.get("recipe")):
        return None
    rootfs = raw.get("rootfs")
    if (
        not isinstance(rootfs, dict)
        or set(rootfs) != {"sha256", "size"}
        or not _is_sha256(rootfs.get("sha256"))
        or not isinstance(rootfs.get("size"), int)
        or isinstance(rootfs.get("size"), bool)
        or int(rootfs["size"]) < 0
    ):
        return None
    initramfs = raw.get("initramfs")
    if initramfs is not None and (
        not isinstance(initramfs, dict)
        or set(initramfs) != {"sha256", "size"}
        or not _is_sha256(initramfs.get("sha256"))
        or not isinstance(initramfs.get("size"), int)
        or isinstance(initramfs.get("size"), bool)
        or int(initramfs["size"]) < 0
    ):
        return None
    return raw


def receipt_matches(output: Path, recipe: str) -> bool:
    """Return whether one success receipt and rootfs still match exactly."""
    raw = _read_receipt(output)
    if raw is None or raw.get("recipe") != recipe:
        return False
    try:
        return raw == _receipt_data(output, recipe)
    except SystemExit:
        return False


def write_receipt(output: Path, recipe: str) -> None:
    """Atomically publish a successful rootfs receipt after the cpio exists."""
    if output.is_symlink() or not output.is_dir():
        fail(f"rootfs output directory is invalid: {output}")
    encoded = (json.dumps(_receipt_data(output, recipe), sort_keys=True) + "\n").encode()
    replace_file_atomically(output / RECEIPT_NAME, encoded, 0o600, sync=False)


def trusted_receipt_identity(output: Path, recipe: str) -> dict[str, str]:
    """Return the identity of an exact receipt whose rootfs still verifies."""
    if not receipt_matches(output, recipe):
        fail("rootfs causal receipt is missing, stale or invalid")
    receipt = output / RECEIPT_NAME
    return {"recipe": recipe, "sha256": sha256_file(receipt)}
