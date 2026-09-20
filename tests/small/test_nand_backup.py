# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral checks for read-only NAND backup publication."""

from __future__ import annotations

import contextlib
import hashlib
import io
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Any, BinaryIO
from unittest import mock

from fplinux_cli import nand_backup


class NandBackupTests(unittest.TestCase):
    """Protect the exact raw stream boundary and its local publication result."""

    def setUp(self) -> None:
        """Create one isolated destination for each backup attempt."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.destination = self.directory / "nand.raw"

    def test_complete_stream_is_atomically_published_with_private_mode(self) -> None:
        """Only the exact completed stream replaces the requested output."""
        payload = bytes(range(256)) * 8 + b"O" * 128
        session = {"session_id": "selected"}

        def stream_remote(
            actual_session: dict[str, Any],
            command: str,
            destination: BinaryIO,
            *,
            timeout: float,
        ) -> None:
            self.assertIs(actual_session, session)
            self.assertEqual(command, "exec dd if=/dev/ums9117-nand-raw bs=65280")
            self.assertEqual(timeout, 15 * 60)
            destination.write(payload)

        connect = mock.Mock(return_value=(SimpleNamespace(stream_remote=stream_remote), session))
        with contextlib.redirect_stdout(io.StringIO()) as stdout:
            result = nand_backup.backup_nand(
                connect,
                self.destination,
                raw_device="/dev/ums9117-nand-raw",
                raw_page_bytes=2176,
                raw_page_count=1,
            )

        self.assertEqual(result, self.destination)
        self.assertEqual(self.destination.read_bytes(), payload)
        self.assertEqual(self.destination.stat().st_mode & 0o777, 0o600)
        self.assertIn(hashlib.sha256(payload).hexdigest(), stdout.getvalue())
        self.assertEqual(list(self.directory.glob(".nand.raw.*")), [])
        connect.assert_called_once_with()

    def test_incomplete_stream_preserves_previous_output_and_removes_temporary(self) -> None:
        """A truncated device read cannot replace a previous complete image."""
        previous = b"previous complete image"
        self.destination.write_bytes(previous)

        def stream_remote(
            _session: dict[str, Any],
            _command: str,
            destination: BinaryIO,
            *,
            timeout: float,
        ) -> None:
            del timeout
            destination.write(b"short")

        connect = mock.Mock(return_value=(SimpleNamespace(stream_remote=stream_remote), {}))
        with self.assertRaisesRegex(SystemExit, "expected 2176 bytes, got 5"):
            nand_backup.backup_nand(
                connect,
                self.destination,
                raw_device="/dev/ums9117-nand-raw",
                raw_page_bytes=2176,
                raw_page_count=1,
            )

        self.assertEqual(self.destination.read_bytes(), previous)
        self.assertEqual(list(self.directory.glob(".nand.raw.*")), [])

    def test_invalid_output_is_rejected_before_session_acquisition(self) -> None:
        """Local validation does not touch a phone when output cannot be published."""
        connect = mock.Mock()
        missing_parent = self.directory / "missing" / "nand.raw"

        with self.assertRaisesRegex(SystemExit, "output directory is missing or invalid"):
            nand_backup.backup_nand(
                connect, missing_parent, raw_device="/dev/ums9117-nand-raw", raw_page_bytes=2176
            )

        connect.assert_not_called()

    def test_target_backup_acquires_the_exact_selected_session(self) -> None:
        """The target entry point binds the raw stream to the selected build identity."""
        payload = b"complete"

        def stream_remote(
            _session: dict[str, Any],
            _command: str,
            destination: BinaryIO,
            *,
            timeout: float,
        ) -> None:
            del timeout
            destination.write(payload)

        with (
            mock.patch(
                "fplinux_cli.commands.current_target_ssh_session",
                return_value=(SimpleNamespace(stream_remote=stream_remote), {}),
            ) as acquire,
            self.assertRaisesRegex(SystemExit, "expected 142606336 bytes, got 8"),
        ):
            nand_backup.backup_target_nand(
                "nokia-ta1618",
                self.destination,
                profile="microsd-uboot",
            )

        self.assertFalse(self.destination.exists())
        acquire.assert_called_once_with("nokia-ta1618", profile="microsd-uboot")

    def test_64_byte_oob_stream_is_not_padded_to_128_bytes(self) -> None:
        """A DS35M1GA physical page retains exactly its 64 spare bytes."""
        payload = b"M" * 2048 + bytes(range(64))

        def stream_remote(
            _session: dict[str, Any],
            command: str,
            destination: BinaryIO,
            *,
            timeout: float,
        ) -> None:
            del timeout
            self.assertEqual(command, "exec dd if=/dev/ums9117-nand-raw bs=63360")
            destination.write(payload)

        connect = mock.Mock(return_value=(SimpleNamespace(stream_remote=stream_remote), {}))
        with contextlib.redirect_stdout(io.StringIO()):
            nand_backup.backup_nand(
                connect,
                self.destination,
                raw_device="/dev/ums9117-nand-raw",
                raw_page_bytes=2112,
                raw_page_count=1,
            )
        self.assertEqual(self.destination.read_bytes(), payload)

    def test_target_without_a_reader_is_rejected_before_connecting(self) -> None:
        """A missing board declaration cannot fall back to another board's reader."""
        with (
            mock.patch.object(nand_backup, "load_target", return_value={}),
            mock.patch("fplinux_cli.commands.current_target_ssh_session") as acquire,
            self.assertRaisesRegex(SystemExit, "not supported for target no-reader"),
        ):
            nand_backup.backup_target_nand("no-reader", self.destination)

        acquire.assert_not_called()


if __name__ == "__main__":
    unittest.main()
