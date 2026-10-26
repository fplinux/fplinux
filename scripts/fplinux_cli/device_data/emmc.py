# SPDX-License-Identifier: GPL-2.0-only
"""Admit GPT partitions and extract selected originals without copying a disk image."""

from __future__ import annotations

import hashlib
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from dataclasses import asdict, dataclass
from itertools import pairwise
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli.runtime.state import private_directory

if TYPE_CHECKING:
    from collections.abc import Sequence
    from typing import BinaryIO

_SECTOR_BYTES = 512
_GPT_HEADER = struct.Struct("<8sIIIIQQQQ16sQIII")
_READER = Path(__file__).with_name("emmc-files") / "read_files.py"


class EmmcError(ValueError):
    """A disk layout or selected original does not satisfy the supported source contract."""


@dataclass(frozen=True)
class Partition:
    """One named user-area interval admitted from both GPT copies."""

    name: str
    offset: int
    size: int


@dataclass(frozen=True)
class FileSelection:
    """One required original and its ordered filesystem source locations."""

    name: str
    locations: tuple[tuple[str, str], ...]
    max_bytes: int


def validate_file_selections(value: object) -> tuple[FileSelection, ...]:
    """Admit the same target-owned names and locations for saved and live sources."""
    if not isinstance(value, list) or not value:
        message = "eMMC file selection must be a non-empty list"
        raise EmmcError(message)
    selected = []
    names: set[str] = set()
    for entry in value:
        if not isinstance(entry, dict) or set(entry) != {"name", "locations", "max_bytes"}:
            message = "eMMC file selection requires name, locations and max_bytes"
            raise EmmcError(message)
        name, locations, maximum = entry["name"], entry["locations"], entry["max_bytes"]
        if (
            not isinstance(name, str)
            or not name
            or name in {".", ".."}
            or PurePosixPath(name).name != name
            or "\\" in name
            or ":" in name
            or "\0" in name
            or name in names
        ):
            message = "eMMC original names must be unique safe filenames"
            raise EmmcError(message)
        if type(maximum) is not int or maximum <= 0:
            raise EmmcError(f"eMMC original {name} requires a positive max_bytes")
        if not isinstance(locations, list) or not locations:
            raise EmmcError(f"eMMC original {name} requires source locations")
        admitted = []
        for location in locations:
            if (
                not isinstance(location, list)
                or len(location) != 2
                or not all(isinstance(part, str) and part for part in location)
            ):
                raise EmmcError(f"eMMC original {name} has an invalid source location")
            partition, path = location
            if (
                "/" in partition
                or "\\" in partition
                or "\0" in partition
                or not path.startswith("/")
                or "\\" in path
                or "\0" in path
                or any(part in {"", ".", ".."} for part in path[1:].split("/"))
            ):
                raise EmmcError(f"eMMC original {name} has an unsafe source location")
            admitted.append((partition, path))
        names.add(name)
        selected.append(FileSelection(name, tuple(admitted), maximum))
    return tuple(selected)


class PartitionReader(io.RawIOBase):
    """Borrow a seekable image while exposing only one admitted partition interval."""

    def __init__(self, source: BinaryIO, partition: Partition) -> None:
        """Retain the caller's image handle without taking ownership of it."""
        super().__init__()
        if partition.offset < 0 or partition.size <= 0:
            message = "eMMC partition must have a positive bounded extent"
            raise EmmcError(message)
        self.source = source
        self.partition = partition
        self.position = 0

    def readable(self) -> bool:
        """Expose read-only access to standard buffered I/O."""
        return True

    def seekable(self) -> bool:
        """Allow filesystem metadata lookups within the partition."""
        return True

    def tell(self) -> int:
        """Report the current partition-relative position."""
        return self.position

    def seek(self, offset: int, whence: int = io.SEEK_SET) -> int:
        """Reject positions outside the admitted partition."""
        if whence == io.SEEK_CUR:
            offset += self.position
        elif whence == io.SEEK_END:
            offset += self.partition.size
        elif whence != io.SEEK_SET:
            message = "unsupported eMMC partition seek mode"
            raise EmmcError(message)
        if not 0 <= offset <= self.partition.size:
            message = "seek is outside the eMMC partition"
            raise EmmcError(message)
        self.position = offset
        return offset

    def readinto(self, buffer: object) -> int:
        """Limit buffered reads to this partition and reject a truncated backing file."""
        view = memoryview(buffer)  # type: ignore[arg-type]  # RawIOBase accepts writable buffers.
        size = min(len(view), self.partition.size - self.position)
        self.source.seek(self.partition.offset + self.position)
        data = self.source.read(size)
        if len(data) != size:
            message = "eMMC partition is truncated"
            raise EmmcError(message)
        view[:size] = data
        self.position += size
        return size


@dataclass(frozen=True)
class _Gpt:
    current: int
    alternate: int
    first: int
    last: int
    guid: bytes
    entries_lba: int
    entries_count: int
    entry_size: int
    entries_crc: int


def _read_at(source: BinaryIO, offset: int, length: int) -> bytes:
    source.seek(offset)
    data = source.read(length)
    if len(data) != length:
        message = "eMMC GPT is truncated"
        raise EmmcError(message)
    return data


def _header(source: BinaryIO, lba: int, sectors: int) -> _Gpt:
    data = bytearray(_read_at(source, lba * _SECTOR_BYTES, _SECTOR_BYTES))
    signature, revision, size, crc, reserved, *fields = _GPT_HEADER.unpack_from(data)
    if (
        signature != b"EFI PART"
        or revision != 0x10000
        or not _GPT_HEADER.size <= size <= _SECTOR_BYTES
        or reserved != 0
    ):
        message = "eMMC GPT header is invalid"
        raise EmmcError(message)
    struct.pack_into("<I", data, 16, 0)
    if zlib.crc32(data[:size]) != crc:
        message = "eMMC GPT header CRC does not match"
        raise EmmcError(message)
    header = _Gpt(*fields)
    if (
        header.current != lba
        or header.alternate != (sectors - 1 if lba == 1 else 1)
        or not 2 <= header.first <= header.last < sectors - 1
        or header.guid == bytes(16)
        or header.entries_count < 1
        or header.entry_size < 128
        or header.entry_size > _SECTOR_BYTES
        or header.entry_size % 128
    ):
        message = "eMMC GPT header layout is invalid"
        raise EmmcError(message)
    entry_start = header.entries_lba * _SECTOR_BYTES
    entry_end = entry_start + header.entries_count * header.entry_size
    if lba == 1:
        valid = 2 * _SECTOR_BYTES <= entry_start < entry_end <= header.first * _SECTOR_BYTES
    else:
        valid = (header.last + 1) * _SECTOR_BYTES <= entry_start < entry_end <= lba * _SECTOR_BYTES
    if not valid:
        message = "eMMC GPT entries overlap storage outside their reserved region"
        raise EmmcError(message)
    return header


def _entries(source: BinaryIO, header: _Gpt) -> tuple[dict[str, Partition], bytes]:
    checksum = 0
    digest = hashlib.sha256()
    partitions = {}
    intervals: list[tuple[int, int]] = []
    source.seek(header.entries_lba * _SECTOR_BYTES)
    for _index in range(header.entries_count):
        entry = source.read(header.entry_size)
        if len(entry) != header.entry_size:
            message = "eMMC GPT entries are truncated"
            raise EmmcError(message)
        checksum = zlib.crc32(entry, checksum)
        digest.update(entry)
        if entry[:16] == bytes(16):
            continue
        first, last = struct.unpack_from("<QQ", entry, 32)
        if not header.first <= first <= last <= header.last:
            message = "eMMC GPT partition extent is invalid"
            raise EmmcError(message)
        intervals.append((first, last))
        try:
            name = entry[56:128].decode("utf-16-le").split("\0", 1)[0]
        except UnicodeDecodeError as error:
            message = "eMMC GPT partition name is invalid"
            raise EmmcError(message) from error
        if name:
            if name in partitions:
                raise EmmcError(f"eMMC GPT repeats partition name {name}")
            partitions[name] = Partition(
                name, first * _SECTOR_BYTES, (last - first + 1) * _SECTOR_BYTES
            )
    if checksum != header.entries_crc:
        message = "eMMC GPT partition-array CRC does not match"
        raise EmmcError(message)
    intervals.sort()
    if any(left[1] >= right[0] for left, right in pairwise(intervals)):
        message = "eMMC GPT partitions overlap"
        raise EmmcError(message)
    return partitions, digest.digest()


def read_gpt(source: BinaryIO) -> dict[str, Partition]:
    """Require matching primary and backup GPT metadata before exposing named partitions."""
    source.seek(0, io.SEEK_END)
    size = source.tell()
    if size % _SECTOR_BYTES or size < 6 * _SECTOR_BYTES:
        message = "eMMC user-area image must contain complete 512-byte sectors"
        raise EmmcError(message)
    sectors = size // _SECTOR_BYTES
    primary = _header(source, 1, sectors)
    backup = _header(source, sectors - 1, sectors)
    if (
        primary.first,
        primary.last,
        primary.guid,
        primary.entries_count,
        primary.entry_size,
        primary.entries_crc,
    ) != (
        backup.first,
        backup.last,
        backup.guid,
        backup.entries_count,
        backup.entry_size,
        backup.entries_crc,
    ):
        message = "eMMC primary and backup GPT describe different layouts"
        raise EmmcError(message)
    partitions, digest = _entries(source, primary)
    _backup_partitions, backup_digest = _entries(source, backup)
    if digest != backup_digest:
        message = "eMMC primary and backup GPT partition arrays differ"
        raise EmmcError(message)
    return partitions


def _python_command() -> list[str]:
    """Relaunch a bundled musl interpreter through its own loader on a glibc host."""
    executable = Path(sys.executable)
    runtime = executable.parent.parent
    loader = runtime / "lib/ld-musl-x86_64.so.1"
    if os.name == "posix" and executable.parent.name == "bin" and loader.is_file():
        return [str(loader), "--library-path", str(runtime / "lib"), str(executable)]
    return [str(executable)]


def extract_files(
    image: Path,
    selections: Sequence[FileSelection],
    *,
    python_modules: Path,
) -> dict[str, bytes]:
    """Return every selected original from an isolated reader with pinned Python modules."""
    declarations = [asdict(selection) for selection in selections]
    admitted = validate_file_selections(json.loads(json.dumps(declarations)))
    if image.is_symlink() or not image.is_file():
        message = "eMMC backup must be a regular user-area image"
        raise EmmcError(message)
    for package in ("ext4", "cachetools", "crcmod"):
        if not (python_modules / package / "__init__.py").is_file():
            raise EmmcError(f"pinned eMMC reader package is missing: {package}")
    with tempfile.TemporaryDirectory(prefix="fplinux-emmc-") as temporary:
        output = private_directory(Path(temporary))
        request = {
            "image": str(image.resolve()),
            "output": str(output),
            "selections": declarations,
        }
        try:
            result = subprocess.run(
                [
                    *_python_command(),
                    "-B",
                    "-I",
                    "-S",
                    str(_READER),
                    str(python_modules.resolve()),
                ],
                input=json.dumps(request),
                capture_output=True,
                text=True,
                check=False,
                timeout=120,
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise EmmcError(f"eMMC file reader failed: {error}") from error
        if result.returncode:
            raise EmmcError(result.stderr.strip() or "eMMC file extraction failed")
        originals = {}
        for selection in admitted:
            path = output / selection.name
            if (
                path.is_symlink()
                or not path.is_file()
                or not 0 < path.stat().st_size <= selection.max_bytes
            ):
                raise EmmcError(f"eMMC reader did not return required original {selection.name}")
            originals[selection.name] = path.read_bytes()
        return originals
