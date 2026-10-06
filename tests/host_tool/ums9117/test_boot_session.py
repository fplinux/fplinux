# SPDX-License-Identifier: GPL-2.0-only
"""Linked host characterization of bounded boot-session personalization."""

from __future__ import annotations

import base64
import tempfile
import unittest
import zlib
from pathlib import Path
from typing import ClassVar

from tests import ROOT
from tests.process import run_process

BOOTSTRAP = ROOT / "platforms/ums9117/bootstrap"
HARNESS = ROOT / "tests/host_tool/ums9117/boot-session.c"
SESSION_ID = bytes.fromhex(
    "01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 "
    "11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f 20"
)
SEED = bytes.fromhex(
    "80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f "
    "90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f "
    "a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af "
    "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
)
CLIENT_KEY = b"AAAAC3NzaC1lZDI1NTE5AAAAIAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8g"
USB_CONFIG = b"SERIAL=test-session\nHOST_IP=10.2.3.1\nPHONE_IP=10.2.3.2\n".ljust(256, b"\0")


def _seal_record(record: bytearray) -> bytes:
    """Seal test-owned wire bytes with the standard-library CRC implementation."""
    record[508:512] = zlib.crc32(record[:508]).to_bytes(4, "little")
    return bytes(record)


def _record() -> bytes:
    """Construct a literal 512-byte session with no production fixture helpers."""
    record = bytearray(512)
    record[:8] = b"FPLSESS\0"
    record[12:16] = b"\x00\x02\x00\x00"
    record[16:48] = SESSION_ID
    record[48:112] = SEED
    record[112:180] = CLIENT_KEY
    record[180:436] = USB_CONFIG
    return _seal_record(record)


def _tree() -> bytes:
    """Provide distinct marker slots and nul-terminated property names."""
    tree = bytearray(b"Z" * 2048)
    for offset, name in (
        (12, b"rng-seed\0"),
        (48, b"fplinux,ssh-client-key\0"),
        (96, b"fplinux,session-id\0"),
        (144, b"fplinux,usb-session\0"),
    ):
        tree[offset : offset + len(name)] = name
    tree[256:320] = b"\xa1" * 64
    tree[384:452] = b"\xb2" * 68
    tree[512:544] = b"\xc3" * 32
    tree[640:896] = b"\xd4" * 256
    return bytes(tree)


class BootSessionHostTests(unittest.TestCase):
    """Validate bounded copies and atomic rejection without physical wrappers."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Link the actual C99 operation separately from the host fixture peer."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        work = Path(cls.temporary.name)
        includes = work / "include"
        includes.mkdir()
        (includes / "ums9117-common").symlink_to(ROOT / "platforms/ums9117/common")
        cls.executable = work / "boot-session"
        run_process(
            [
                "cc",
                "-std=c99",
                "-pedantic",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-ffunction-sections",
                f"-I{BOOTSTRAP}",
                f"-I{includes}",
                str(HARNESS),
                str(BOOTSTRAP / "boot-session.c"),
                "-Wl,--gc-sections",
                "-o",
                str(cls.executable),
            ],
            name="compile bounded boot-session host peer",
            timeout=30,
            check=True,
        )

    def copy_session(
        self,
        record: bytes,
        tree: bytes,
        *,
        mode: str = "copy",
        record_bytes: int = 512,
        tree_bytes: int = 2048,
    ) -> tuple[int, bytes, bytes]:
        """Collect status, full tree and guarded output from one isolated call."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            (work / "record.bin").write_bytes(record)
            (work / "tree.bin").write_bytes(tree)
            result = run_process(
                [str(self.executable), mode, str(record_bytes), str(tree_bytes)],
                name="run bounded boot-session host peer",
                cwd=work,
                timeout=10,
                check=True,
            )
            output = (work / "result.bin").read_bytes()
        self.assertEqual(len(output), 2080)
        return int(result.stdout), output[:2048], output[2048:]

    def assert_rejected(  # noqa: PLR0913 -- rejection inputs stay visible.
        self,
        record: bytes,
        tree: bytes,
        status: int,
        *,
        mode: str = "copy",
        record_bytes: int = 512,
        tree_bytes: int = 2048,
    ) -> None:
        """Leave the tree and acknowledgement untouched when a request is invalid."""
        actual = self.copy_session(
            record,
            tree,
            mode=mode,
            record_bytes=record_bytes,
            tree_bytes=tree_bytes,
        )
        self.assertEqual(actual, (status, tree, b"u" * 32))

    def test_valid_record_copies_only_slots_and_session_output(self) -> None:
        """Copy all four values while preserving record, guards and other tree bytes."""
        tree = _tree()
        expected = bytearray(tree)
        expected[256:320] = SEED
        expected[384:452] = CLIENT_KEY
        expected[512:544] = SESSION_ID
        expected[640:896] = USB_CONFIG
        for tree_bytes in (896, 2048):
            with self.subTest(tree_bytes=tree_bytes):
                self.assertEqual(
                    self.copy_session(_record(), tree, tree_bytes=tree_bytes),
                    (0, bytes(expected), SESSION_ID),
                )

    def test_bad_layout_output_and_truncated_tree_are_atomic(self) -> None:
        """Reject invalid extents, alignment and missing output before any copying."""
        record, tree = _record(), _tree()
        for record_bytes in (0, 511, 513):
            with self.subTest(record_bytes=record_bytes):
                self.assert_rejected(record, tree, 1, record_bytes=record_bytes)
        self.assert_rejected(record, tree, 1, mode="unaligned")
        self.assert_rejected(record, tree, 11, mode="no-output")
        self.assert_rejected(record, tree, 11, mode="no-output", record_bytes=0)
        for tree_bytes in (0, 63, 895):
            with self.subTest(tree_bytes=tree_bytes):
                self.assert_rejected(record, tree, 10, tree_bytes=tree_bytes)

    def test_corrupt_record_and_empty_material_are_atomic(self) -> None:
        """Reject malformed header, CRC, reserved fields and absent session material."""
        for label, start, replacement, status in (
            ("magic", 0, b"X", 2),
            ("size", 12, b"\xff\x01\x00\x00", 3),
            ("header reserved", 8, b"\x01", 5),
            ("tail reserved", 507, b"\x01", 5),
            ("empty session", 16, b"\0" * 32, 6),
            ("empty seed", 48, b"\0" * 64, 7),
        ):
            with self.subTest(label=label):
                record = bytearray(_record())
                record[start : start + len(replacement)] = replacement
                self.assert_rejected(_seal_record(record), _tree(), status)
        for offset in (16, 111, 179, 435, 508, 511):
            with self.subTest(tampered_byte=offset):
                record = bytearray(_record())
                record[offset] ^= 1
                self.assert_rejected(bytes(record), _tree(), 4)

    def test_malformed_client_key_and_usb_text_are_atomic(self) -> None:
        """Invalid SSH key encodings and USB text or padding never personalize the tree."""
        zero_key = b"AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        for label, key in (
            ("invalid alphabet", b"!" + CLIENT_KEY[1:]),
            ("base64 padding", CLIENT_KEY[:-1] + b"="),
            ("wrong key type", b"B" + CLIENT_KEY[1:]),
            ("zero public key", zero_key),
            (
                "wrong key length",
                base64.b64encode(b"\0\0\0\x0bssh-ed25519\0\0\0\x1f" + SESSION_ID),
            ),
        ):
            with self.subTest(key=label):
                record = bytearray(_record())
                record[112:180] = key
                self.assert_rejected(_seal_record(record), _tree(), 8)
        for label, config in (
            ("empty", b"\0" * 256),
            ("unterminated", b"A" * 255 + b"\n"),
            ("missing newline", b"A\0".ljust(256, b"\0")),
            ("control byte", b"A\t\n\0".ljust(256, b"\0")),
            ("non-ASCII", b"A\x80\n\0".ljust(256, b"\0")),
            ("nonzero padding", b"A\n\0X".ljust(256, b"\0")),
        ):
            with self.subTest(config=label):
                record = bytearray(_record())
                record[180:436] = config
                self.assert_rejected(_seal_record(record), _tree(), 9)

    def test_absent_or_ambiguous_marker_slots_are_atomic(self) -> None:
        """Every marker slot must be present exactly once, including overlapping runs."""
        for offset, marker, length in (
            (256, 0xA1, 64),
            (384, 0xB2, 68),
            (512, 0xC3, 32),
            (640, 0xD4, 256),
        ):
            for defect in ("absent", "duplicate", "overlapping"):
                with self.subTest(marker=marker, defect=defect):
                    tree = bytearray(_tree())
                    if defect == "absent":
                        tree[offset] = 0x5A
                    elif defect == "duplicate":
                        tree[1024 : 1024 + length] = bytes([marker]) * length
                    else:
                        tree[offset + length] = marker
                    self.assert_rejected(_record(), bytes(tree), 10)

    def test_absent_or_duplicate_property_names_are_atomic(self) -> None:
        """Nul-terminated names must identify each slot uniquely before values change."""
        for offset, name in (
            (12, b"rng-seed\0"),
            (48, b"fplinux,ssh-client-key\0"),
            (96, b"fplinux,session-id\0"),
            (144, b"fplinux,usb-session\0"),
        ):
            for defect in ("absent", "duplicate", "unterminated"):
                with self.subTest(name=name, defect=defect):
                    tree = bytearray(_tree())
                    if defect == "absent":
                        tree[offset] = ord("X")
                    elif defect == "duplicate":
                        tree[1400 : 1400 + len(name)] = name
                    else:
                        tree[offset + len(name) - 1] = ord("X")
                    self.assert_rejected(_record(), bytes(tree), 10)


if __name__ == "__main__":
    unittest.main()
