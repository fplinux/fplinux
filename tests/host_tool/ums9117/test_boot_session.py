# SPDX-License-Identifier: GPL-2.0-only
"""Linked host characterization of bounded boot-session personalization."""

from __future__ import annotations

import base64
import tempfile
import zlib
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

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


class BootSessionHostTests:
    """Validate bounded copies and atomic rejection without physical wrappers."""

    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_peer(cls) -> Iterator[None]:
        """Link the actual C99 operation separately from the host fixture peer."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
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
            yield

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
        assert (len(output)) == (2080)
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
        assert (actual) == ((status, tree, b"u" * 32))

    @pytest.mark.parametrize("tree_bytes", [896, 2048], ids=["exact-slot-extent", "whole-tree"])
    def test_valid_record_copies_only_slots_and_session_output(self, tree_bytes: int) -> None:
        """Copy all four values while preserving record, guards and other tree bytes."""
        tree = _tree()
        expected = bytearray(tree)
        expected[256:320] = SEED
        expected[384:452] = CLIENT_KEY
        expected[512:544] = SESSION_ID
        expected[640:896] = USB_CONFIG
        assert self.copy_session(_record(), tree, tree_bytes=tree_bytes) == (
            0,
            bytes(expected),
            SESSION_ID,
        )

    @pytest.mark.parametrize(
        ("mode", "record_bytes", "tree_bytes", "status"),
        [
            pytest.param("copy", 0, 2048, 1, id="empty-record"),
            pytest.param("copy", 511, 2048, 1, id="short-record"),
            pytest.param("copy", 513, 2048, 1, id="long-record"),
            pytest.param("unaligned", 512, 2048, 1, id="unaligned-record"),
            pytest.param("no-output", 512, 2048, 11, id="missing-output"),
            pytest.param("no-output", 0, 2048, 11, id="missing-output-and-record"),
            pytest.param("copy", 512, 0, 10, id="empty-tree"),
            pytest.param("copy", 512, 63, 10, id="short-tree"),
            pytest.param("copy", 512, 895, 10, id="truncated-slot"),
        ],
    )
    def test_bad_layout_output_and_truncated_tree_are_atomic(
        self, mode: str, record_bytes: int, tree_bytes: int, status: int
    ) -> None:
        """Reject invalid extents, alignment and missing output before any copying."""
        record, tree = _record(), _tree()
        self.assert_rejected(
            record, tree, status, mode=mode, record_bytes=record_bytes, tree_bytes=tree_bytes
        )

    @pytest.mark.parametrize(
        ("start", "replacement", "status"),
        [
            pytest.param(0, b"X", 2, id="bad-magic"),
            pytest.param(12, b"\xff\x01\x00\x00", 3, id="bad-size"),
            pytest.param(8, b"\x01", 5, id="header-reserved"),
            pytest.param(507, b"\x01", 5, id="tail-reserved"),
            pytest.param(16, b"\0" * 32, 6, id="empty-session"),
            pytest.param(48, b"\0" * 64, 7, id="empty-seed"),
            *[
                pytest.param(offset, None, 4, id=f"bad-crc-byte-{offset}")
                for offset in (16, 111, 179, 435, 508, 511)
            ],
        ],
    )
    def test_corrupt_record_and_empty_material_are_atomic(
        self, start: int, replacement: bytes | None, status: int
    ) -> None:
        """Reject malformed header, CRC, reserved fields and absent session material."""
        record = bytearray(_record())
        if replacement is None:
            record[start] ^= 1
            malformed = bytes(record)
        else:
            record[start : start + len(replacement)] = replacement
            malformed = _seal_record(record)
        self.assert_rejected(malformed, _tree(), status)

    @pytest.mark.parametrize(
        ("start", "replacement", "status"),
        [
            pytest.param(112, b"!" + CLIENT_KEY[1:], 8, id="key-invalid-alphabet"),
            pytest.param(112, CLIENT_KEY[:-1] + b"=", 8, id="key-base64-padding"),
            pytest.param(112, b"B" + CLIENT_KEY[1:], 8, id="key-wrong-type"),
            pytest.param(
                112,
                b"AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                8,
                id="key-zero-material",
            ),
            pytest.param(
                112,
                base64.b64encode(b"\0\0\0\x0bssh-ed25519\0\0\0\x1f" + SESSION_ID),
                8,
                id="key-wrong-length",
            ),
            pytest.param(180, b"\0" * 256, 9, id="usb-empty"),
            pytest.param(180, b"A" * 255 + b"\n", 9, id="usb-unterminated"),
            pytest.param(180, b"A\0".ljust(256, b"\0"), 9, id="usb-missing-newline"),
            pytest.param(180, b"A\t\n\0".ljust(256, b"\0"), 9, id="usb-control-byte"),
            pytest.param(180, b"A\x80\n\0".ljust(256, b"\0"), 9, id="usb-non-ascii"),
            pytest.param(180, b"A\n\0X".ljust(256, b"\0"), 9, id="usb-nonzero-padding"),
        ],
    )
    def test_malformed_client_key_and_usb_text_are_atomic(
        self, start: int, replacement: bytes, status: int
    ) -> None:
        """Invalid SSH key encodings and USB text or padding never personalize the tree."""
        record = bytearray(_record())
        record[start : start + len(replacement)] = replacement
        self.assert_rejected(_seal_record(record), _tree(), status)

    @pytest.mark.parametrize("defect", ["absent", "duplicate", "overlapping"])
    @pytest.mark.parametrize(
        ("offset", "marker", "length"),
        [(256, 0xA1, 64), (384, 0xB2, 68), (512, 0xC3, 32), (640, 0xD4, 256)],
        ids=["rng-seed", "client-key", "session-id", "usb-session"],
    )
    def test_absent_or_ambiguous_marker_slots_are_atomic(
        self, offset: int, marker: int, length: int, defect: str
    ) -> None:
        """Every marker slot must be present exactly once, including overlapping runs."""
        tree = bytearray(_tree())
        if defect == "absent":
            tree[offset] = 0x5A
        elif defect == "duplicate":
            tree[1024 : 1024 + length] = bytes([marker]) * length
        else:
            tree[offset + length] = marker
        self.assert_rejected(_record(), bytes(tree), 10)

    @pytest.mark.parametrize("defect", ["absent", "duplicate", "unterminated"])
    @pytest.mark.parametrize(
        ("offset", "name"),
        [
            (12, b"rng-seed\0"),
            (48, b"fplinux,ssh-client-key\0"),
            (96, b"fplinux,session-id\0"),
            (144, b"fplinux,usb-session\0"),
        ],
        ids=["rng-seed", "client-key", "session-id", "usb-session"],
    )
    def test_absent_or_duplicate_property_names_are_atomic(
        self, offset: int, name: bytes, defect: str
    ) -> None:
        """Nul-terminated names must identify each slot uniquely before values change."""
        tree = bytearray(_tree())
        if defect == "absent":
            tree[offset] = ord("X")
        elif defect == "duplicate":
            tree[1400 : 1400 + len(name)] = name
        else:
            tree[offset + len(name) - 1] = ord("X")
        self.assert_rejected(_record(), bytes(tree), 10)
