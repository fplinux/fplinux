# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Host commands for the Nokia TA-1618 read-only NAND profile."""

from __future__ import annotations

import argparse
import hashlib
import os
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Any, BinaryIO, NoReturn, Protocol, cast

RAW_DEVICE = "/dev/ta1618-nand-raw"
RAW_PAGE_BYTES = 2048 + 128
RAW_PAGE_COUNT = 65536
RAW_BYTES = RAW_PAGE_COUNT * RAW_PAGE_BYTES
READ_BATCH_BYTES = 30 * RAW_PAGE_BYTES
BACKUP_TIMEOUT_SECONDS = 15 * 60


class SshTransport(Protocol):
    """Binary-stream operation supplied by the exact profile bundle."""

    def stream_remote(
        self,
        session: dict[str, Any],
        command: str,
        destination: BinaryIO,
        *,
        timeout: float,
    ) -> None:
        """Write remote stdout directly into the supplied local file."""


SessionFactory = Callable[[], tuple[SshTransport, dict[str, Any]]]


def fail(message: str) -> NoReturn:
    """Stop one profile command without publishing partial output."""
    raise SystemExit(f"nand-ro-lab: {message}")


def sha256_file(path: Path) -> str:
    """Hash one completed local backup without loading it into memory."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def backup(ssh: SshTransport, session: dict[str, Any], output_name: str) -> None:
    """Stream and atomically publish one complete physical NAND image."""
    destination = Path(output_name)
    if destination.name in {"", ".", ".."}:
        fail("output must name a file")
    parent = destination.parent
    if not parent.is_dir():
        fail(f"output directory is missing or invalid: {parent}")
    if destination.exists() and not destination.is_file():
        fail(f"output is not a regular file: {destination}")

    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=parent,
            prefix=f".{destination.name}.",
            mode="w+b",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            temporary.chmod(0o600)
            ssh.stream_remote(
                session,
                f"exec dd if={RAW_DEVICE} bs={READ_BATCH_BYTES}",
                cast("BinaryIO", stream),
                timeout=BACKUP_TIMEOUT_SECONDS,
            )
            stream.flush()
            os.fsync(stream.fileno())

        actual_size = temporary.stat().st_size
        if actual_size != RAW_BYTES:
            fail(f"incomplete raw image: expected {RAW_BYTES} bytes, got {actual_size}")
        digest = sha256_file(temporary)
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)

    print(
        f"NAND backup verified: {destination} ({RAW_BYTES} bytes, sha256={digest})",
        flush=True,
    )


def run(connect: SessionFactory, arguments: list[str]) -> None:
    """Parse and execute one command inside the selected profile namespace."""
    parser = argparse.ArgumentParser(prog="fplinux profile nokia-ta1618 nand-ro-lab")
    commands = parser.add_subparsers(dest="command", required=True)
    backup_parser = commands.add_parser(
        "nand-backup", help="save the complete physical NAND image"
    )
    backup_parser.add_argument("output", metavar="OUTPUT")
    options = parser.parse_args(arguments)

    if options.command == "nand-backup":
        ssh, session = connect()
        backup(ssh, session, options.output)
        return
    raise AssertionError(f"unhandled profile command: {options.command}")
