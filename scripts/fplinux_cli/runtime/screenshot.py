# SPDX-License-Identifier: GPL-2.0-only
"""Capture one validated PPM frame from the already selected phone."""

from __future__ import annotations

import re
import tempfile
from typing import TYPE_CHECKING, Any, Protocol, cast

from fplinux_cli.common import fail, replace_file_atomically

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path
    from typing import BinaryIO


_MAX_DIMENSION = 1024
_HEADER = re.compile(rb"P6\n([1-9][0-9]{0,3}) ([1-9][0-9]{0,3})\n255\n")
_MAX_BYTES = len(b"P6\n1024 1024\n255\n") + 3 * _MAX_DIMENSION**2


class ScreenshotTransport(Protocol):
    """The binary SSH command boundary shared by the Linux and Windows transports."""

    def stream_remote(
        self,
        session: dict[str, Any],
        command: str,
        destination: BinaryIO,
        *,
        timeout: float,
        progress: Callable[[int], None],
    ) -> None:
        """Run a single non-interactive command with binary stdout."""


def _check_size(size: int) -> None:
    if size > _MAX_BYTES:
        fail("The phone's screenshot exceeds the PPM size limit")


def capture_screenshot(
    transport: ScreenshotTransport, session: dict[str, Any], path: Path
) -> None:
    """Publish only a complete PPM frame from a successful remote reader."""
    with tempfile.TemporaryFile(mode="w+b", dir=path.parent) as temporary:
        stream = cast("BinaryIO", temporary)
        transport.stream_remote(
            session,
            "exec /usr/libexec/fplinux/screenshot",
            stream,
            timeout=15,
            progress=_check_size,
        )
        stream.seek(0)
        data = stream.read(_MAX_BYTES + 1)
    _check_size(len(data))
    header = _HEADER.match(data)
    if header is None:
        fail("The phone did not return a P6 PPM screenshot")
    width, height = (int(value) for value in header.groups())
    if width > _MAX_DIMENSION or height > _MAX_DIMENSION:
        fail("The phone returned unsupported screenshot dimensions")
    if len(data) != header.end() + 3 * width * height:
        fail("The phone returned an incomplete or invalid screenshot frame")
    replace_file_atomically(path, data, 0o600)
