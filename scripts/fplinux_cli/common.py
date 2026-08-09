# SPDX-License-Identifier: GPL-2.0-only
"""Shared primitives for the repository-local FPLinux CLI."""

from __future__ import annotations

import hashlib
import shlex
import subprocess
from pathlib import Path, PurePosixPath
from typing import Any, NoReturn

ROOT = Path(__file__).resolve().parents[2]
ZIP_TIMESTAMP = (2026, 7, 24, 19, 0, 0)


def display_text(value: object) -> str:
    """Hide the host checkout location in user-facing output."""
    text = str(value)
    root = str(ROOT)
    if text == root:
        return "<source-root>"
    return text.replace(f"{root}/", "<source-root>/")


def fail(message: str) -> NoReturn:
    raise SystemExit(f"fplinux: {display_text(message)}")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def payload_digest(files: dict[str, bytes], executables: set[str]) -> str:
    value = hashlib.sha256()
    for name in sorted(files):
        value.update(name.encode())
        value.update(b"\0")
        mode = 0o100755 if name in executables else 0o100644
        value.update(mode.to_bytes(4, "big"))
        value.update(files[name])
        value.update(b"\0")
    return value.hexdigest()


def relative_name(value: object, *, field: str) -> str:
    if not isinstance(value, str) or not value:
        fail(f"{field} must be a non-empty relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or value != path.as_posix():
        fail(f"{field} must be a normalized relative path: {value}")
    return value


def target_source(target: str, config: dict[str, Any], field: str) -> Path:
    relative = relative_name(config.get(field), field=f"target {target} {field}")
    path = ROOT / "targets" / target / relative
    if path.is_symlink() or not path.is_file():
        fail(f"target {target} {field} is missing or invalid: {path}")
    return path


def run(command: list[str]) -> None:
    display = [display_text(argument) for argument in command]
    print("+", shlex.join(display), flush=True)
    result = subprocess.run(command, check=False)
    if result.returncode:
        fail(f"command failed with exit status {result.returncode}: {display[0]}")
