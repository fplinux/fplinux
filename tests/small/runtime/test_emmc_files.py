# SPDX-License-Identifier: GPL-2.0-only
"""Admission of the bounded file archive received through authenticated SSH."""

from __future__ import annotations

import io
import shlex
import tarfile
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.device_data.emmc import EmmcError, FileSelection
from fplinux_cli.runtime import emmc_files

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence
    from typing import Any, BinaryIO


def file_archive(entries: Sequence[tuple[str, bytes, bytes]], *, pax: bool = False) -> bytes:
    """Make test-owned tar records, including entries the reader must reject."""
    output = io.BytesIO()
    with tarfile.open(
        fileobj=output, mode="w:", format=tarfile.PAX_FORMAT if pax else tarfile.USTAR_FORMAT
    ) as archive:
        for name, contents, kind in entries:
            member = tarfile.TarInfo(name)
            member.type = kind
            member.size = len(contents)
            if pax:
                member.pax_headers = {"comment": "extra metadata"}
            archive.addfile(member, io.BytesIO(contents))
    return output.getvalue()


class ArchivePhone:
    """Replace SSH with one byte stream; retain the real command and local file lifetime."""

    def __init__(self, payload: bytes, *, failure: Exception | None = None) -> None:
        """Retain controlled output and its independently selected completion result."""
        self.payload = payload
        self.failure = failure
        self.destination: BinaryIO | None = None
        self.command: list[str] = []

    def stream_remote(
        self,
        session: dict[str, Any],
        command: str,
        destination: BinaryIO,
        *,
        timeout: float,
        progress: Callable[[int], None] | None = None,
    ) -> None:
        """Produce controlled bytes and optionally report an unsuccessful SSH completion."""
        assert session == {"selected": "phone"}
        assert 0 < timeout <= 120
        self.destination = destination
        self.command = shlex.split(command)
        destination.write(self.payload)
        if progress is not None:
            progress(len(self.payload))
        if self.failure is not None:
            raise self.failure


def test_read_originals_preserves_bytes_and_quotes_source_arguments() -> None:
    """A selected filename and source with shell characters remain literal argv values."""
    phone = ArchivePhone(
        file_archive([("audio 'original.bin", b"\0\xffaudio\n", tarfile.REGTYPE)])
    )
    selections = (
        FileSelection("audio 'original.bin", (("phone data", "/local/a'$(value).bin"),), 100),
    )

    actual = emmc_files.read_originals(phone, {"selected": "phone"}, "/dev/mmcblk12", selections)

    assert actual == {"audio 'original.bin": b"\0\xffaudio\n"}
    assert phone.command[:2] == ["sh", "-c"]
    assert phone.command[3:] == [
        "fplinux-emmc-files",
        "/dev/mmcblk12",
        "1",
        "audio 'original.bin",
        "100",
        "1",
        "phone data",
        "/local/a'$(value).bin",
        "audio 'original.bin",
    ]
    assert phone.destination is not None
    assert phone.destination.closed


@pytest.mark.parametrize(
    ("entries", "message"),
    [
        ([], "missing required originals"),
        ([("audio.bin", b"", tarfile.REGTYPE)], "invalid size"),
        ([("audio.bin", b"12345", tarfile.REGTYPE)], "invalid size"),
        ([("../audio.bin", b"x", tarfile.REGTYPE)], "unexpected or repeated"),
        ([("extra.bin", b"x", tarfile.REGTYPE)], "unexpected or repeated"),
        ([("audio.bin", b"", tarfile.SYMTYPE)], "unexpected or repeated"),
        (
            [("audio.bin", b"x", tarfile.REGTYPE), ("audio.bin", b"y", tarfile.REGTYPE)],
            "unexpected or repeated",
        ),
    ],
    ids=["missing", "empty", "oversize", "escape", "extra", "symlink", "duplicate"],
)
def test_read_originals_rejects_incomplete_or_unselected_archives(
    entries: Sequence[tuple[str, bytes, bytes]], message: str
) -> None:
    """Only each exact requested regular file may cross the acquisition boundary."""
    phone = ArchivePhone(file_archive(entries))

    with pytest.raises(EmmcError, match=message):
        emmc_files.read_originals(
            phone,
            {"selected": "phone"},
            "/dev/mmcblk0",
            (FileSelection("audio.bin", (("data", "/a"),), 4),),
        )

    assert phone.destination is not None
    assert phone.destination.closed


@pytest.mark.parametrize("kind", ["pax", "short-payload", "short-trailer", "trailing-data"])
def test_read_originals_rejects_archive_extensions_and_truncation(kind: str) -> None:
    """A successful SSH exit cannot admit hidden headers, missing bytes or extra output."""
    payload = file_archive([("audio.bin", b"original", tarfile.REGTYPE)], pax=kind == "pax")
    if kind == "short-payload":
        payload = payload[:515]
    elif kind == "short-trailer":
        payload = payload[:1024]
    elif kind == "trailing-data":
        payload += b"unexpected output"
    phone = ArchivePhone(payload)

    with pytest.raises(EmmcError):
        emmc_files.read_originals(
            phone,
            {"selected": "phone"},
            "/dev/mmcblk0",
            (FileSelection("audio.bin", (("data", "/a"),), 20),),
        )

    assert phone.destination is not None
    assert phone.destination.closed


def test_read_originals_preserves_remote_failure_after_complete_archive() -> None:
    """A cleanup or SSH failure remains a failure even after all file bytes arrived."""
    failure = RuntimeError("remote unmount failed")
    phone = ArchivePhone(
        file_archive([("audio.bin", b"original", tarfile.REGTYPE)]), failure=failure
    )

    with pytest.raises(RuntimeError, match="remote unmount failed") as caught:
        emmc_files.read_originals(
            phone,
            {"selected": "phone"},
            "/dev/mmcblk0",
            (FileSelection("audio.bin", (("data", "/a"),), 20),),
        )

    assert caught.value is failure
    assert phone.destination is not None
    assert phone.destination.closed


def test_read_originals_stops_an_excessive_stream_and_closes_local_file() -> None:
    """A transport progress event bounds an oversized response before archive parsing."""
    phone = ArchivePhone(b"x" * 12000)

    with pytest.raises(EmmcError, match="exceeds the selected size limits"):
        emmc_files.read_originals(
            phone,
            {"selected": "phone"},
            "/dev/mmcblk0",
            (FileSelection("audio.bin", (("data", "/a"),), 4),),
        )

    assert phone.destination is not None
    assert phone.destination.closed


@pytest.mark.parametrize(
    "device", ["/dev/mmcblk0p1", "/dev/mmcblk0boot0", "/dev/sda", "/dev/mmcblk0;true"]
)
def test_read_originals_rejects_partition_or_alias_before_contacting_phone(device: str) -> None:
    """Acquisition requires the explicitly selected whole MMC card."""
    phone = ArchivePhone(b"")

    with pytest.raises(EmmcError, match="whole /dev/mmcblkN card"):
        emmc_files.read_originals(
            phone,
            {"selected": "phone"},
            device,
            (FileSelection("audio.bin", (("data", "/a"),), 4),),
        )

    assert phone.destination is None
