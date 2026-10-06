# SPDX-License-Identifier: GPL-2.0-only
"""Fetch exact declared files with checksum, size and offline policy."""

from __future__ import annotations

import hashlib
import shutil
import tempfile
import urllib.request
from pathlib import Path

from fplinux_cli.common import fail, sha256_file


def download_locked_file(  # noqa: PLR0913 -- checksum, size and offline policy are distinct inputs.
    url: str,
    digest: str,
    destination: Path,
    *,
    offline: bool = False,
    algorithm: str = "sha256",
    size: int | None = None,
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
        fail(f"offline locked input is missing or mismatched: {url}: {destination}")
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent,
            prefix=f".{destination.name}.",
            delete=False,
        ) as output:
            temporary = Path(output.name)
            request = urllib.request.Request(  # noqa: S310 -- HTTPS is required above.
                url,
                headers={"User-Agent": "FPLinux"},
            )
            with urllib.request.urlopen(  # noqa: S310 -- HTTPS is required above.
                request,
                timeout=60,
            ) as response:
                shutil.copyfileobj(response, output)
        actual = _locked_file_digest(temporary, algorithm=algorithm)
        if actual != digest:
            fail(
                f"locked download {algorithm.upper()} mismatch: {url}: "
                f"expected {digest}, received {actual}"
            )
        if size is not None and temporary.stat().st_size != size:
            fail(f"locked download size mismatch: {url}: expected {size}")
        temporary.replace(destination)
        temporary = None
    except OSError as error:
        fail(f"locked download failed: {url}: {error}")
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
