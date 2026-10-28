# SPDX-License-Identifier: GPL-2.0-only
"""Fetch exact declared files with checksum, size and offline policy."""

from __future__ import annotations

import hashlib
import shutil
import tempfile
import urllib.request
from http.client import IncompleteRead
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import fail, sha256_file

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import NoReturn


def download_locked_file(  # noqa: PLR0913 -- checksum, size and offline policy are distinct inputs.
    url: str,
    digest: str,
    destination: Path,
    *,
    offline: bool = False,
    algorithm: str = "sha256",
    size: int | None = None,
    setup_command: str = "./fplinux setup --all",
) -> Path:
    """Fetch one declared checksum match atomically, or require saved bytes offline."""
    if not url.startswith("https://"):
        fail("locked download URL must use HTTPS")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() or (destination.exists() and not destination.is_file()):
        fail(f"invalid locked download path: {destination}")
    if (
        destination.is_file()
        and (size is None or destination.stat().st_size == size)
        and _locked_file_digest(destination, algorithm=algorithm) == digest
    ):
        return destination
    if offline:
        fail(
            f"offline locked input is missing or mismatched: {url}: {destination}; "
            f"run {setup_command}"
        )

    def verify(path: Path) -> None:
        actual = _locked_file_digest(path, algorithm=algorithm)
        if actual != digest:
            fail(
                f"locked download {algorithm.upper()} mismatch: {url}: "
                f"expected {digest}, received {actual}"
            )
        if size is not None and path.stat().st_size != size:
            fail(f"locked download size mismatch: {url}: expected {size}")

    def report_failure(error: OSError) -> NoReturn:
        fail(f"locked download failed: {url}: {error}")

    return transfer_verified_file(url, destination, verify=verify, on_os_error=report_failure)


def transfer_verified_file(
    url: str,
    destination: Path,
    *,
    verify: Callable[[Path], None],
    on_os_error: Callable[[OSError], NoReturn] | None = None,
) -> Path:
    """Publish a download only after the caller verifies its closed temporary file."""
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent,
            prefix=f".{destination.name}.",
            delete=False,
        ) as output:
            temporary = Path(output.name)
            request = urllib.request.Request(  # noqa: S310 -- callers require HTTPS before transfer.
                url,
                headers={"User-Agent": "FPLinux"},
            )
            for attempt in range(1, 4):
                try:
                    with urllib.request.urlopen(  # noqa: S310 -- callers require HTTPS.
                        request,
                        timeout=60,
                    ) as response:
                        shutil.copyfileobj(response, output)
                    break
                except IncompleteRead as error:
                    if attempt == 3:
                        message = "HTTP response interrupted after 3 attempts"
                        raise OSError(message) from error
                    output.seek(0)
                    output.truncate()
        verify(temporary)
        temporary.replace(destination)
        temporary = None
    except OSError as error:
        if on_os_error is None:
            raise
        on_os_error(error)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return destination


def _locked_file_digest(path: Path, *, algorithm: str) -> str:
    """Use the checksum algorithm declared for the original downloaded artifact."""
    if algorithm == "sha256":
        return sha256_file(path)
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()
