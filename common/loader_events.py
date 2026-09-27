# SPDX-License-Identifier: GPL-2.0-only
"""Flushed JSON-lines progress for one RAM loader invocation."""

from __future__ import annotations

import json
from contextlib import contextmanager, suppress
from datetime import UTC, datetime
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path
    from typing import TextIO


class LoaderEvents:
    """Write public progress without copying private session material."""

    def __init__(
        self,
        stream: TextIO | None,
        *,
        target: str,
        profile: str | None,
        build_type: str,
    ) -> None:
        """Bind the stream to the selected bundle identity."""
        self.stream = stream
        self.target = target
        self.profile = profile
        self.build_type = build_type
        self.stage = "preflight"

    def emit(
        self, event: str, *, cause: str | None = None, exit_status: int | None = None
    ) -> None:
        """Publish a complete line before the caller continues loading."""
        record: dict[str, str | int | None] = {
            "event": event,
            "target": self.target,
            "profile": self.profile,
            "build_type": self.build_type,
            "time": datetime.now(UTC).isoformat(),
        }
        if event == "failure":
            record.update(stage=self.stage, cause=cause, exit_status=exit_status)
        else:
            self.stage = event
        if self.stream is not None:
            self.stream.write(json.dumps(record, separators=(",", ":")) + "\n")
            self.stream.flush()


@contextmanager
def record_events(
    path: Path | None,
    *,
    target: str,
    profile: str | None,
    build_type: str,
) -> Iterator[LoaderEvents]:
    """Overwrite an optional stream and preserve the loader's failure status."""
    stream = path.open("w", encoding="utf-8") if path is not None else None
    events = LoaderEvents(stream, target=target, profile=profile, build_type=build_type)
    try:
        yield events
    except BaseException as error:
        status = 1
        cause = "unexpected-error"
        if isinstance(error, SystemExit):
            status = error.code if isinstance(error.code, int) else int(error.code is not None)
            cause = "interrupted" if status in (129, 130, 143) else "command-failed"
        elif isinstance(error, KeyboardInterrupt):
            status = 130
            cause = "interrupted"
        elif isinstance(error, OSError):
            cause = "io-error"
        if status:
            # An unavailable event destination must not replace the loading error.
            with suppress(OSError):
                events.emit("failure", cause=cause, exit_status=status)
        raise
    finally:
        if stream is not None:
            stream.close()
