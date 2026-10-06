# SPDX-License-Identifier: GPL-2.0-only
"""Structured RAM-loader records through real temporary output files."""

from __future__ import annotations

import json
import tempfile
from datetime import datetime, timedelta
from pathlib import Path

import pytest

from common.loader_events import record_events


class LoaderEventTests:
    """Consumers see complete records while the producer is still running."""

    def test_event_is_flushed_with_exact_selected_identity(self) -> None:
        """A waiting consumer need not wait for the loader to close its file."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "events.jsonl"
            path.write_text("previous invocation\n", encoding="utf-8")
            with record_events(
                path, target="nokia-ta1618", profile="console", build_type="debug"
            ) as events:
                events.emit("waiting-for-device")
                lines = path.read_text(encoding="utf-8").splitlines()
                assert (len(lines)) == (1)
                record = json.loads(lines[0])
                timestamp = datetime.fromisoformat(record.pop("time"))
                offset = timestamp.utcoffset()
                assert (offset) == (timedelta(0))
                assert (record) == (
                    {
                        "event": "waiting-for-device",
                        "target": "nokia-ta1618",
                        "profile": "console",
                        "build_type": "debug",
                    }
                )

    @pytest.mark.parametrize(
        ("error", "status", "cause"),
        [
            pytest.param(
                SystemExit("private diagnostic"), 1, "command-failed", id="diagnostic-exit"
            ),
            pytest.param(SystemExit(7), 7, "command-failed", id="numeric-exit"),
            pytest.param(SystemExit(143), 143, "interrupted", id="interrupted-exit"),
            pytest.param(OSError("private path"), 1, "io-error", id="io-error"),
        ],
    )
    def test_failure_preserves_status_and_omits_exception_material(
        self, error: BaseException, status: int, cause: str
    ) -> None:
        """A consumer gets the failed boundary without private diagnostic text."""
        with (
            tempfile.TemporaryDirectory() as temporary,
        ):
            path = Path(temporary) / "events.jsonl"

            def fail_loader_stage() -> None:
                """Let the recorder observe the stage error before the assertion catches it."""
                with record_events(
                    path, target="nokia-ta1618", profile=None, build_type="release"
                ) as events:
                    events.emit("ram-loader-complete")
                    raise error

            with pytest.raises(type(error)) as caught:
                fail_loader_stage()
            assert (caught.value) is (error)
            records = [json.loads(line) for line in path.read_text().splitlines()]
            assert (len(records)) == (2)
            failure = records[1]
            failure.pop("time")
            assert (failure) == (
                {
                    "event": "failure",
                    "target": "nokia-ta1618",
                    "profile": None,
                    "build_type": "release",
                    "stage": "ram-loader-complete",
                    "cause": cause,
                    "exit_status": status,
                }
            )

    def test_disabled_events_preserve_loading_error(self) -> None:
        """The default human-only mode needs no output destination."""
        message = "failed"

        def fail_loading_without_events() -> None:
            """Propagate the loading error through the disabled recorder."""
            with record_events(
                None, target="nokia-ta1618", profile=None, build_type="release"
            ) as events:
                events.emit("waiting-for-device")
                raise SystemExit(message)

        with pytest.raises(SystemExit, match="failed"):
            fail_loading_without_events()
