# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral checks for NAND identification and read-only backup publication."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from typing import Any, BinaryIO
from unittest import mock

from fplinux_cli import nand_backup
from fplinux_cli.device_data import NandGeometry


def _geometry_report(  # noqa: PLR0913 -- each reported value stays visible at the call site.
    *,
    id_bytes: str,
    chip: str,
    page_main_bytes: int,
    oob_bytes: int,
    pages_per_block: int,
    block_count: int,
    raw_bytes: int,
    source: str = "table",
) -> str:
    """Render one reader report in the kernel's documented key=value order."""
    return (
        f"id_bytes={id_bytes}\n"
        f"chip={chip}\n"
        f"page_main_bytes={page_main_bytes}\n"
        f"oob_bytes={oob_bytes}\n"
        f"pages_per_block={pages_per_block}\n"
        f"block_count={block_count}\n"
        f"raw_bytes={raw_bytes}\n"
        f"geometry_source={source}\n"
        "feature_a0=0x00000000\n"
        "feature_b0=0x00000010\n"
        "feature_c0=0x00000000\n"
    )


ONE_PAGE_128_OOB = _geometry_report(
    id_bytes="a1b1",
    chip="demo-128",
    page_main_bytes=2048,
    oob_bytes=128,
    pages_per_block=1,
    block_count=1,
    raw_bytes=2176,
)
ONE_PAGE_64_OOB = _geometry_report(
    id_bytes="e521",
    chip="demo-64",
    page_main_bytes=2048,
    oob_bytes=64,
    pages_per_block=1,
    block_count=1,
    raw_bytes=2112,
)
UNKNOWN_CHIP = _geometry_report(
    id_bytes="c8c1",
    chip="unknown",
    page_main_bytes=0,
    oob_bytes=0,
    pages_per_block=0,
    block_count=0,
    raw_bytes=0,
    source="unknown",
)


class FakePhone:
    """Replace the authenticated SSH transport with one geometry report and one raw stream.

    It cannot show that the phone kernel identifies a chip when its reader is opened.
    """

    def __init__(self, geometry: str, payload: bytes) -> None:
        """Keep the reader's report, the raw bytes and every remote command it receives."""
        self.geometry = geometry
        self.payload = payload
        self.commands: list[str] = []
        self.stream_timeout: float | None = None

    def stream_remote(
        self,
        session: dict[str, Any],
        command: str,
        destination: BinaryIO,
        *,
        timeout: float,
    ) -> None:
        """Answer a raw read with the payload and any other command with the report."""
        del session
        self.commands.append(command)
        if command.startswith("exec dd "):
            self.stream_timeout = timeout
            destination.write(self.payload)
        else:
            destination.write(self.geometry.encode("ascii"))

    def raw_reads(self) -> list[str]:
        """Return the commands that read NAND pages."""
        return [command for command in self.commands if command.startswith("exec dd ")]


class NandBackupTests(unittest.TestCase):
    """Protect the exact raw stream boundary and its local publication result."""

    def setUp(self) -> None:
        """Create one isolated destination for each backup attempt."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.enterContext(mock.patch("fplinux_cli.output.ROOT", self.directory))
        self.destination = self.directory / "nand.raw"
        self.receipt = self.directory / "nand.raw.json"

    def _backup(
        self,
        phone: FakePhone,
        *,
        declared_id: int | None = None,
        declared_raw_page_bytes: int | None = None,
    ) -> str:
        with contextlib.redirect_stdout(io.StringIO()) as stdout:
            nand_backup.backup_nand(
                lambda: (phone, {"session_id": "selected"}),
                self.destination,
                target="demo-phone",
                raw_device="/dev/ums9117-nand-raw",
                declared_id=declared_id,
                declared_raw_page_bytes=declared_raw_page_bytes,
            )
        return stdout.getvalue()

    def test_complete_stream_and_geometry_receipt_are_published_with_private_mode(self) -> None:
        """The reported size bounds the stream, and the receipt records that exact image."""
        payload = bytes(range(256)) * 8 + b"O" * 128
        phone = FakePhone(ONE_PAGE_128_OOB, payload)

        output = self._backup(phone, declared_id=0xB1A1, declared_raw_page_bytes=2176)

        digest = hashlib.sha256(payload).hexdigest()
        self.assertEqual(self.destination.read_bytes(), payload)
        self.assertEqual(self.destination.stat().st_mode & 0o777, 0o600)
        self.assertEqual(
            json.loads(self.receipt.read_text(encoding="utf-8")),
            {
                "id_bytes": "a1b1",
                "chip": "demo-128",
                "page_main_bytes": 2048,
                "oob_bytes": 128,
                "pages_per_block": 1,
                "block_count": 1,
                "raw_bytes": 2176,
                "sha256": digest,
                "target": "demo-phone",
            },
        )
        self.assertEqual(self.receipt.stat().st_mode & 0o777, 0o600)
        self.assertEqual(phone.raw_reads(), ["exec dd if=/dev/ums9117-nand-raw bs=65280"])
        self.assertEqual(phone.stream_timeout, 15 * 60)
        self.assertIn(digest, output)
        self.assertIn("NAND backup saved:", output)
        self.assertNotIn("verified", output)
        self.assertEqual(list(self.directory.glob(".nand.raw.*")), [])

    def test_target_without_declared_chip_takes_its_page_size_from_the_device(self) -> None:
        """A new target streams the reported 64-byte OOB page without padding it to 128."""
        payload = b"M" * 2048 + bytes(range(64))
        phone = FakePhone(ONE_PAGE_64_OOB, payload)

        self._backup(phone)

        self.assertEqual(self.destination.read_bytes(), payload)
        self.assertEqual(phone.raw_reads(), ["exec dd if=/dev/ums9117-nand-raw bs=63360"])
        self.assertEqual(
            json.loads(self.receipt.read_text(encoding="utf-8"))["raw_bytes"],
            2112,
        )

    def test_incomplete_stream_preserves_previous_output_and_receipt(self) -> None:
        """A truncated device read cannot replace a previous complete image or its receipt."""
        self.destination.write_bytes(b"previous complete image")
        self.receipt.write_text("previous receipt\n", encoding="utf-8")

        with self.assertRaisesRegex(SystemExit, "expected 2176 bytes, got 5"):
            self._backup(FakePhone(ONE_PAGE_128_OOB, b"short"))

        self.assertEqual(self.destination.read_bytes(), b"previous complete image")
        self.assertEqual(self.receipt.read_text(encoding="utf-8"), "previous receipt\n")
        self.assertEqual(list(self.directory.glob(".nand.raw.*")), [])

    def test_unidentified_chip_is_refused_before_any_page_is_read(self) -> None:
        """An unknown chip reports its ID bytes but never produces a guessed-size image."""
        phone = FakePhone(UNKNOWN_CHIP, b"must not be read")

        with self.assertRaisesRegex(SystemExit, "does not identify this NAND chip: id_bytes=c8c1"):
            self._backup(phone)

        self.assertEqual(phone.raw_reads(), [])
        self.assertFalse(self.destination.exists())
        self.assertFalse(self.receipt.exists())

    def test_chip_other_than_the_declared_one_is_refused_before_any_page_is_read(self) -> None:
        """A target's declared chip is an expectation that the device must meet."""
        for declared_id, declared_raw_page_bytes in ((0xB1A1, 2176), (0x21E5, 2176)):
            phone = FakePhone(ONE_PAGE_64_OOB, b"must not be read")
            with (
                self.subTest(declared_id=declared_id, page_bytes=declared_raw_page_bytes),
                self.assertRaisesRegex(
                    SystemExit, "reported id_bytes=e521 with 2112-byte pages; target declares"
                ),
            ):
                self._backup(
                    phone,
                    declared_id=declared_id,
                    declared_raw_page_bytes=declared_raw_page_bytes,
                )
            self.assertEqual(phone.raw_reads(), [])
            self.assertFalse(self.destination.exists())

    def test_malformed_geometry_report_is_refused_before_any_page_is_read(self) -> None:
        """The stream length is taken only from a complete, self-consistent report."""
        cases = {
            "missing key": ONE_PAGE_128_OOB.replace("block_count=1\n", ""),
            "duplicate key": ONE_PAGE_128_OOB + "chip=other\n",
            "unexpected key": ONE_PAGE_128_OOB + "planes=1\n",
            "non-decimal size": ONE_PAGE_128_OOB.replace("raw_bytes=2176", "raw_bytes=0x880"),
            "size contradicts layout": ONE_PAGE_128_OOB.replace(
                "raw_bytes=2176", "raw_bytes=4352"
            ),
            "unknown source": ONE_PAGE_128_OOB.replace("=table", "=guess"),
        }
        for name, report in cases.items():
            phone = FakePhone(report, b"must not be read")
            with self.subTest(name), self.assertRaises(SystemExit):
                self._backup(phone)
            self.assertEqual(phone.raw_reads(), [])
            self.assertFalse(self.destination.exists())

    def test_invalid_output_is_rejected_before_session_acquisition(self) -> None:
        """Local validation does not touch a phone when output cannot be published."""
        connect = mock.Mock()
        missing_parent = self.directory / "missing" / "nand.raw"

        with self.assertRaisesRegex(SystemExit, "output directory is missing or invalid"):
            nand_backup.backup_nand(
                connect,
                missing_parent,
                target="demo-phone",
                raw_device="/dev/ums9117-nand-raw",
            )

        connect.assert_not_called()

    def test_target_backup_acquires_the_exact_selected_session(self) -> None:
        """The target entry point binds the raw stream to the selected build identity."""
        phone = FakePhone(
            _geometry_report(
                id_bytes="a1b1",
                chip="demo-128",
                page_main_bytes=2048,
                oob_bytes=128,
                pages_per_block=64,
                block_count=1024,
                raw_bytes=142606336,
            ),
            b"complete",
        )
        with (
            mock.patch(
                "fplinux_cli.cli.runtime.current_target_ssh_session",
                return_value=(phone, {}),
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

    def test_target_without_a_reader_is_rejected_before_connecting(self) -> None:
        """A missing board declaration cannot fall back to another board's reader."""
        with (
            mock.patch.object(nand_backup, "load_target", return_value={}),
            mock.patch("fplinux_cli.cli.runtime.current_target_ssh_session") as acquire,
            self.assertRaisesRegex(SystemExit, "not supported for target no-reader"),
        ):
            nand_backup.backup_target_nand("no-reader", self.destination)

        acquire.assert_not_called()


class NandIdentifyTests(unittest.TestCase):
    """Report what the running reader identifies without saving an image."""

    def test_identify_prints_the_reader_report_unchanged_and_reads_no_pages(self) -> None:
        """An unidentified chip's ID bytes and feature values reach the user as reported."""
        phone = FakePhone(UNKNOWN_CHIP, b"must not be read")
        with (
            tempfile.TemporaryDirectory() as temporary,
            mock.patch("fplinux_cli.output.ROOT", Path(temporary)),
            mock.patch(
                "fplinux_cli.cli.runtime.current_target_ssh_session",
                return_value=(phone, {}),
            ) as acquire,
            contextlib.redirect_stdout(io.StringIO()) as stdout,
            contextlib.redirect_stderr(io.StringIO()),
        ):
            nand_backup.identify_target_nand("inoi-244-modern-4g")

        self.assertEqual(stdout.getvalue(), UNKNOWN_CHIP)
        self.assertEqual(phone.raw_reads(), [])
        self.assertEqual(len(phone.commands), 1)
        self.assertIn("/dev/ums9117-nand-raw", phone.commands[0])
        acquire.assert_called_once_with("inoi-244-modern-4g", profile=None)


class BackupReceiptTests(unittest.TestCase):
    """Bind a saved geometry receipt to the exact bytes it describes."""

    def setUp(self) -> None:
        """Create one small backup and a receipt describing it."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.backup = Path(self.temporary.name) / "saved.bin"
        self.raw = b"R" * 24
        self.backup.write_bytes(self.raw)
        self.receipt = Path(self.temporary.name) / "saved.bin.json"
        self.record: dict[str, object] = {
            "id_bytes": "e521",
            "chip": "demo-64",
            "page_main_bytes": 4,
            "oob_bytes": 2,
            "pages_per_block": 2,
            "block_count": 2,
            "raw_bytes": 24,
            "sha256": hashlib.sha256(self.raw).hexdigest(),
            "target": "demo-phone",
        }

    def _write(self, record: dict[str, object]) -> None:
        self.receipt.write_text(json.dumps(record), encoding="utf-8")

    def test_receipt_returns_the_recorded_geometry_or_nothing_when_absent(self) -> None:
        """Only a present receipt supplies geometry; its absence is not an error here."""
        self.assertIsNone(nand_backup.read_backup_geometry(self.backup, self.raw))

        self._write(self.record)

        self.assertEqual(
            nand_backup.read_backup_geometry(self.backup, self.raw),
            NandGeometry(
                id_bytes="e521",
                chip="demo-64",
                page_main_bytes=4,
                oob_bytes=2,
                pages_per_block=2,
                block_count=2,
                raw_bytes=24,
            ),
        )

    def test_receipt_that_does_not_describe_these_bytes_is_refused(self) -> None:
        """A receipt from another backup, or a damaged one, cannot supply a page layout."""
        cases: dict[str, tuple[dict[str, object], str]] = {
            "larger backup": (
                {**self.record, "block_count": 4, "raw_bytes": 48},
                "describes a 48-byte backup",
            ),
            "same size, other bytes": (
                {**self.record, "sha256": hashlib.sha256(b"S" * 24).hexdigest()},
                "describes different backup bytes",
            ),
            "size contradicts layout": ({**self.record, "oob_bytes": 4}, "is inconsistent"),
            "missing field": (
                {key: value for key, value in self.record.items() if key != "chip"},
                "must contain exactly",
            ),
            "boolean size": ({**self.record, "pages_per_block": True}, "must be an integer"),
        }
        for name, (record, message) in cases.items():
            self._write(record)
            with self.subTest(name), self.assertRaisesRegex(SystemExit, message):
                nand_backup.read_backup_geometry(self.backup, self.raw)

    def test_receipt_written_by_backup_is_accepted_for_the_same_bytes(self) -> None:
        """The backup writer and the saved-dump reader agree on one receipt format."""
        payload = b"P" * 2112
        with (
            mock.patch("fplinux_cli.output.ROOT", Path(self.temporary.name)),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            nand_backup.backup_nand(
                lambda: (FakePhone(ONE_PAGE_64_OOB, payload), {}),
                self.backup,
                target="demo-phone",
                raw_device="/dev/ums9117-nand-raw",
            )

        self.assertEqual(
            nand_backup.read_backup_geometry(self.backup, payload),
            NandGeometry(
                id_bytes="e521",
                chip="demo-64",
                page_main_bytes=2048,
                oob_bytes=64,
                pages_per_block=1,
                block_count=1,
                raw_bytes=2112,
            ),
        )


if __name__ == "__main__":
    unittest.main()
