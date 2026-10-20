# SPDX-License-Identifier: GPL-2.0-only
"""Flushed JSON-lines progress for one RAM loader invocation."""

from __future__ import annotations

import json
import os
import sys
import time
from contextlib import contextmanager, redirect_stderr, redirect_stdout, suppress
from datetime import UTC, datetime
from typing import TYPE_CHECKING

from fplinux_cli.common import replace_file_atomically

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator
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


class RunLog:
    """One private text log and one journal, excluding interactive terminal contents."""

    def __init__(
        self, root: Path, *, protect: Callable[[Path], None], visible: TextIO | None = None
    ) -> None:
        """Create the journal before admitting any bundle content."""
        self.started = datetime.now(UTC)
        self.monotonic = time.monotonic()
        stamp = self.started.strftime("%Y-%m-%d_%H%M%S_%f")
        self.directory = root / stamp
        self.directory.mkdir(mode=0o700)
        protect(self.directory)
        self.path = self.directory / "runtime.log"
        descriptor = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_APPEND, 0o600)
        self.stream = os.fdopen(descriptor, "w", encoding="utf-8", buffering=1)
        self.visible = sys.stdout if visible is None else visible
        self.stage_started = self.monotonic
        self.stage = "admission"
        self.status = "running"
        self._write_journal()

    def _write_journal(self, *, finished: datetime | None = None) -> None:
        record = {
            "started_at": self.started.isoformat(),
            "finished_at": finished.isoformat() if finished is not None else None,
            "status": self.status,
            "pid": os.getpid(),
            "parent": None,
            "stages": [{"name": "runtime", "log": self.path.name, "status": self.status}],
            "runtime_stage": self.stage,
        }
        replace_file_atomically(
            self.directory / "run.json", (json.dumps(record) + "\n").encode(), 0o600
        )

    def message(self, text: str) -> None:
        """Show progress and retain the same message with its UTC timestamp."""
        print(text, file=self.visible, flush=True)
        self.record(text)

    def record(self, text: str) -> None:
        """Retain diagnostics without writing to a command's data stream."""
        print(f"{datetime.now(UTC).isoformat()} {text}", file=self.stream, flush=True)

    def event(self, stage: str, *, invitation: str) -> None:
        """Record each loader stage without repeating the text log in journal metadata."""
        now = time.monotonic()
        print(f"Stage {self.stage}: {now - self.stage_started:.2f}s", file=self.stream)
        self.stage, self.stage_started = stage, now
        self._write_journal()
        messages = {
            "host-preparation": "Checking the computer and USB access...",
            "network-preparation": "Choosing the private USB connection...",
            "session-preparation": "Creating secure access for this boot...",
            "payload-preparation": "Preparing the RAM image...",
            "waiting-for-device": invitation,
            "device-found": "Phone detected...",
            "ram-loader-start": "Loading the phone into RAM; keep USB connected...",
            "ram-loader-complete": "Phone loaded into RAM.",
            "linux-start": "Starting Linux...",
            "linux-transition": "Waiting for the phone's USB network and SSH...",
            "ssh-ready": "Secure phone connection is ready.",
            "driver-setup": "Follow the Windows USB driver setup window.",
            "driver-ready": "Phone USB driver is ready.",
            "usb-access-setup": "Setting up USB access; Linux may ask for your password once.",
        }
        if stage in messages:
            self.message(messages[stage])

    @contextmanager
    def capture(self) -> Iterator[TextIO]:
        """Capture backend diagnostics while progress still uses the inherited terminal."""
        with redirect_stdout(self.stream), redirect_stderr(self.stream):
            yield self.stream

    def close(self, status: str) -> None:
        """Publish the final result before releasing the launch lock."""
        self.status = status
        print(f"Launch ended after {time.monotonic() - self.monotonic:.2f}s", file=self.stream)
        self._write_journal(finished=datetime.now(UTC))
        self.stream.close()
