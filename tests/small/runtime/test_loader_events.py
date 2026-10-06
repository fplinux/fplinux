# SPDX-License-Identifier: GPL-2.0-only
"""Structured RAM-loader records through real temporary output files."""

from __future__ import annotations

import json
import tempfile
import unittest
from datetime import datetime, timedelta
from pathlib import Path

from common.loader_events import record_events


class LoaderEventTests(unittest.TestCase):
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
                self.assertEqual(len(lines), 1)
                record = json.loads(lines[0])
                timestamp = datetime.fromisoformat(record.pop("time"))
                offset = timestamp.utcoffset()
                self.assertEqual(offset, timedelta(0))
                self.assertEqual(
                    record,
                    {
                        "event": "waiting-for-device",
                        "target": "nokia-ta1618",
                        "profile": "console",
                        "build_type": "debug",
                    },
                )

    def test_failure_preserves_status_and_omits_exception_material(self) -> None:
        """A consumer gets the failed boundary without private diagnostic text."""
        for error, status, cause in (
            (SystemExit("private diagnostic"), 1, "command-failed"),
            (SystemExit(7), 7, "command-failed"),
            (SystemExit(143), 143, "interrupted"),
            (OSError("private path"), 1, "io-error"),
        ):
            with (
                self.subTest(status=status, cause=cause),
                tempfile.TemporaryDirectory() as temporary,
            ):
                path = Path(temporary) / "events.jsonl"
                with (
                    self.assertRaises(type(error)) as caught,
                    record_events(
                        path, target="nokia-ta1618", profile=None, build_type="release"
                    ) as events,
                ):
                    events.emit("ram-loader-complete")
                    raise error
                self.assertIs(caught.exception, error)
                records = [json.loads(line) for line in path.read_text().splitlines()]
                self.assertEqual(len(records), 2)
                failure = records[1]
                failure.pop("time")
                self.assertEqual(
                    failure,
                    {
                        "event": "failure",
                        "target": "nokia-ta1618",
                        "profile": None,
                        "build_type": "release",
                        "stage": "ram-loader-complete",
                        "cause": cause,
                        "exit_status": status,
                    },
                )

    def test_disabled_events_preserve_loading_error(self) -> None:
        """The default human-only mode needs no output destination."""
        with (
            self.assertRaisesRegex(SystemExit, "failed"),
            record_events(
                None, target="nokia-ta1618", profile=None, build_type="release"
            ) as events,
        ):
            events.emit("waiting-for-device")
            message = "failed"
            raise SystemExit(message)
