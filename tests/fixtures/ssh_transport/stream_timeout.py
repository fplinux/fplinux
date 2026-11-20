# SPDX-License-Identifier: GPL-2.0-only
"""Process and clock boundaries for testing real SSH stream deadlines."""

from __future__ import annotations

import subprocess
import time
from contextlib import contextmanager
from typing import TYPE_CHECKING, Any
from unittest import mock

if TYPE_CHECKING:
    from collections.abc import Iterator


class StreamClock:
    """Keep a real clock until the test advances the transport's deadline."""

    def __init__(self, timeout: float) -> None:
        """Set the advance needed to expire the configured deadline."""
        self.timeout = timeout
        self._advance = False
        self.expired_after_progress = False

    def expire_after_progress(self) -> None:
        """Advance the next transport clock read by one full timeout."""
        self._advance = True

    def monotonic(self) -> float:
        """Record whether the transport observed the advance."""
        now = time.monotonic()
        if self._advance:
            self.expired_after_progress = True
            return now + self.timeout
        return now


@contextmanager
def capture_stream_children() -> Iterator[list[subprocess.Popen[bytes]]]:
    """Observe launched children and clean up only after the caller checks reaping."""
    popen = subprocess.Popen
    children: list[subprocess.Popen[bytes]] = []

    def launch(
        *arguments: Any,  # noqa: ANN401 -- transparent subprocess forwarding.
        **options: Any,  # noqa: ANN401 -- transparent subprocess forwarding.
    ) -> subprocess.Popen[bytes]:
        child = popen(*arguments, **options)
        children.append(child)
        return child

    try:
        with mock.patch.object(subprocess, "Popen", side_effect=launch):
            yield children
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
