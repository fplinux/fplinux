# SPDX-License-Identifier: GPL-2.0-only
"""Stream and atomically publish a complete read-only NAND backup."""

from __future__ import annotations

import os
import shlex
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Any, BinaryIO, Protocol, cast

from .common import fail, sha256_file
from .config import load_target

RAW_PAGE_COUNT = 65536
BACKUP_TIMEOUT_SECONDS = 15 * 60


class SshTransport(Protocol):
    """Authenticated binary-stream operation supplied by the selected bundle."""

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


def backup_nand(
    connect: SessionFactory,
    output: Path,
    *,
    raw_device: str,
    raw_page_bytes: int,
    raw_page_count: int = RAW_PAGE_COUNT,
) -> Path:
    """Acquire one exact session and publish a complete physical NAND image."""
    if output.name in {"", ".", ".."}:
        fail("NAND backup output must name a file")
    parent = output.parent
    if parent.is_symlink() or not parent.is_dir():
        fail(f"NAND backup output directory is missing or invalid: {parent}")
    if output.is_symlink() or (output.exists() and not output.is_file()):
        fail(f"NAND backup output is not a regular file: {output}")

    raw_bytes = raw_page_count * raw_page_bytes
    read_batch_bytes = 30 * raw_page_bytes
    ssh, session = connect()
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=parent,
            prefix=f".{output.name}.",
            mode="w+b",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            temporary.chmod(0o600)
            ssh.stream_remote(
                session,
                f"exec dd if={shlex.quote(raw_device)} bs={read_batch_bytes}",
                cast("BinaryIO", stream),
                timeout=BACKUP_TIMEOUT_SECONDS,
            )
            stream.flush()
            os.fsync(stream.fileno())

        actual_size = temporary.stat().st_size
        if actual_size != raw_bytes:
            fail(f"incomplete raw NAND image: expected {raw_bytes} bytes, got {actual_size}")
        digest = sha256_file(temporary)
        temporary.replace(output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)

    print(
        f"NAND backup verified: {output} ({raw_bytes} bytes, sha256={digest})",
        flush=True,
    )
    return output


def backup_target_nand(
    target: str,
    output: Path,
    *,
    profile: str | None = None,
) -> Path:
    """Back up one supported target through its exact current-profile session."""
    nand = load_target(target, profile=profile).get("nand")
    if nand is None:
        fail(f"NAND backup is not supported for target {target}")

    # Import lazily so the backend can be wired by the CLI without an import cycle.
    from .commands import current_target_ssh_session  # noqa: PLC0415

    def connect() -> tuple[SshTransport, dict[str, Any]]:
        ssh, session = current_target_ssh_session(target, profile=profile)
        return cast("SshTransport", ssh), session

    return backup_nand(
        connect, output, raw_device=nand["raw_device"], raw_page_bytes=nand["raw_page_bytes"]
    )
