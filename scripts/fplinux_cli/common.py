# SPDX-License-Identifier: GPL-2.0-only
"""Shared primitives for the repository-local FPLinux CLI."""

from __future__ import annotations

import hashlib
import json
import os
import shlex
import subprocess
import tarfile
import tempfile
import tomllib
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any, NoReturn

if TYPE_CHECKING:
    from collections.abc import Callable

ROOT = Path(__file__).resolve().parents[2]
ZIP_TIMESTAMP = (2026, 7, 24, 19, 0, 0)


def display_text(value: object) -> str:
    """Hide the host checkout location in user-facing output."""
    text = str(value)
    root = str(ROOT)
    if text == root:
        return "<source-root>"
    return text.replace(f"{root}/", "<source-root>/")


def error_message(message: object) -> str:
    """Format a repository command failure without exposing the checkout path."""
    return f"fplinux: {display_text(message)}"


def fail(message: str) -> NoReturn:
    raise SystemExit(error_message(message))


def load_toml(path: Path) -> dict[str, Any]:
    """Read a TOML input and attach its path to a syntax error."""
    try:
        with path.open("rb") as stream:
            return tomllib.load(stream)
    except tomllib.TOMLDecodeError as error:
        fail(f"invalid TOML in {path}: {error}")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def canonical_json_bytes(value: object) -> bytes:
    """Encode deterministic JSON receipts and causal manifests."""
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return (encoded + "\n").encode()


def read_json_object(path: Path) -> dict[str, object] | None:
    """Read a receipt only when it is an ordinary JSON object."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except OSError, ValueError:
        return None
    return value if isinstance(value, dict) else None


def alpine_tar_filter(
    member: tarfile.TarInfo,
    destination: str,
    *,
    on_error: Callable[[str], NoReturn],
) -> tarfile.TarInfo | None:
    """Apply the data filter while retaining Alpine's absolute rootfs links.

    The consumer owns the diagnostic prefix for rejected Alpine links.
    Other rejection errors come directly from the standard data filter.
    """
    if member.issym() or member.islnk():
        target = PurePosixPath(member.linkname)
        if target.is_absolute():
            if ".." in target.parts:
                on_error(f"Alpine minirootfs link escapes the root: {member.name}")
            relative_target = target.as_posix().lstrip("/")
            filtered = tarfile.data_filter(member.replace(linkname=relative_target), destination)
            if filtered is None:
                return None
            return filtered.replace(linkname=member.linkname)
    return tarfile.data_filter(member, destination)


def replace_file_atomically(path: Path, contents: bytes, mode: int, *, sync: bool = True) -> None:
    """Publish one verified regular file without exposing a partial write."""
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=path.parent,
            prefix=f".{path.name}.",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(contents)
            if sync:
                stream.flush()
                os.fsync(stream.fileno())
        temporary.chmod(mode)
        temporary.replace(path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


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
