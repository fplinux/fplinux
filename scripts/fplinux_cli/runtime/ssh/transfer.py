# SPDX-License-Identifier: GPL-2.0-only
"""Verify and publish files pulled through an authenticated SSH transport."""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import is_sha256, sha256_file

from .errors import fail

if TYPE_CHECKING:
    from collections.abc import Callable
    from subprocess import CompletedProcess


def remote_file_metadata(
    remote: str, run_command: Callable[[str], CompletedProcess[str]]
) -> tuple[int, str]:
    """Read the byte count and SHA-256 of a validated, readable phone file."""
    command = (
        f"if [ -f '{remote}' ] && [ -r '{remote}' ]; then "
        f"set -- $(sha256sum '{remote}'); size=$(wc -c < '{remote}'); "
        'printf "%s %s\\n" "$size" "$1"; else exit 44; fi'
    )
    result = run_command(command)
    if result.returncode:
        fail(f"the phone cannot read {remote}")
    fields = result.stdout.strip().split()
    if len(fields) != 2 or not fields[0].isdigit() or not is_sha256(fields[1]):
        fail("the phone returned invalid file metadata")
    return int(fields[0]), fields[1]


def pull_file(
    remote: str,
    destination: Path,
    *,
    metadata: Callable[[], tuple[int, str]],
    receive: Callable[[Path, int], None],
) -> None:
    """Stage the download beside its destination and publish only matching bytes."""
    expected_size, expected_hash = metadata()
    parent = destination.parent
    if parent.is_symlink() or not parent.is_dir():
        fail(f"pull destination directory is missing or invalid: {parent}")
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=parent,
            prefix=f".{destination.name}.",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
        temporary.chmod(0o600)
        receive(temporary, expected_size)
        if temporary.stat().st_size != expected_size or sha256_file(temporary) != expected_hash:
            fail("downloaded file does not match the size and SHA-256 reported by the phone")
        if metadata() != (expected_size, expected_hash):
            fail("pull source changed while it was downloaded")
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    message = f"pull verified: {remote} ({expected_size} bytes, sha256={expected_hash})"
    print(f"{message} -> {destination}", flush=True)
