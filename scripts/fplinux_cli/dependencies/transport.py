# SPDX-License-Identifier: GPL-2.0-only
"""Stream verified input snapshots through split standard tar.zst archives."""

from __future__ import annotations

import io
import re
import shutil
import subprocess
import tarfile
import tempfile
from contextlib import contextmanager, suppress
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import fail, is_sha256, sha256_file

from .snapshots import prepare_directory, read_snapshot

if TYPE_CHECKING:
    from collections.abc import Buffer, Iterator, Sequence
    from typing import IO, Any, BinaryIO


_PART_BYTES = 1024**3
_TRANSPORT_PREFIX = re.compile(r"FPLinux-[0-9a-f]{12}-inputs\Z")


class _SplitWriter(io.RawIOBase):
    """Bound each output file while the tar writer streams compressed bytes."""

    def __init__(self, directory: Path, prefix: str) -> None:
        super().__init__()
        self.directory = directory
        self.prefix = prefix
        self.paths: list[Path] = []
        self._stream: BinaryIO | None = None
        self._written = 0

    def writable(self) -> bool:
        return True

    def write(self, buffer: Buffer, /) -> int:
        data = memoryview(buffer)
        position = 0
        while position < len(data):
            if self._stream is None:
                path = self.directory / f"{self.prefix}.tar.zst.part-{len(self.paths):05d}"
                self._stream = path.open("xb")
                path.chmod(0o600)
                self.paths.append(path)
                self._written = 0
            count = min(len(data) - position, _PART_BYTES - self._written)
            self._stream.write(data[position : position + count])
            self._written += count
            position += count
            if self._written == _PART_BYTES:
                self._stream.close()
                self._stream = None
        return len(data)

    def close(self) -> None:
        if self._stream is not None:
            self._stream.close()
            self._stream = None
        super().close()


@contextmanager
def _zstd_output(command: list[str], *, decompress: bool) -> Iterator[IO[bytes]]:
    """Own the producer and codec pipes, statuses and bounded failure diagnostics."""
    with tempfile.TemporaryFile() as diagnostics:
        producer = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=diagnostics)
        codec: subprocess.Popen[bytes] | None = None
        try:
            if producer.stdout is None:
                fail("dependency transport input pipe is unavailable")
            codec = subprocess.Popen(
                ["zstd", "--quiet", "--stdout", "--decompress" if decompress else "--compress"],
                stdin=producer.stdout,
                stdout=subprocess.PIPE,
                stderr=diagnostics,
            )
            producer.stdout.close()
            if codec.stdout is None:
                fail("dependency transport zstd pipe is unavailable")
            archive_error: tarfile.TarError | None = None
            try:
                yield codec.stdout
            except tarfile.TarError as error:
                archive_error = error
            # Tar readers can finish before the final padding or compressed frame checksum.
            while codec.stdout.read(1024 * 1024):
                pass
            codec_status = codec.wait()
            producer_status = producer.wait()
            if producer_status or codec_status:
                diagnostics.seek(0, io.SEEK_END)
                size = diagnostics.tell()
                diagnostics.seek(max(0, size - 4096))
                detail = diagnostics.read().decode(errors="replace").strip()
                message = (
                    f"dependency transport command failed: {command[0]} exit {producer_status}, "
                    f"zstd exit {codec_status}"
                )
                fail(f"{message}: {detail}" if detail else message)
            if archive_error is not None:
                raise archive_error
        finally:
            for process in (codec, producer):
                if process is None:
                    continue
                if process.poll() is None:
                    with suppress(ProcessLookupError):
                        process.kill()
                process.wait()
                if process.stdout is not None:
                    with suppress(OSError):
                        process.stdout.close()


def _inputs_only_snapshot(directory: Path) -> dict[str, Any]:
    manifest = read_snapshot(directory)
    if manifest["environment"] is not None:
        fail("dependency transport requires an inputs-only snapshot")
    return manifest


def _new_destination(destination: Path) -> Path:
    destination = destination.absolute()
    if destination.exists() or destination.is_symlink():
        fail(f"dependency transport destination already exists: {destination}")
    prepare_directory(destination.parent)
    return destination


def export_snapshot(snapshot: Path, output: Path, *, prefix: str) -> Path:
    """Publish release-named parts and their index without copying unrelated files."""
    if _TRANSPORT_PREFIX.fullmatch(prefix) is None:
        fail("dependency transport prefix must identify one FPLinux release")
    manifest = _inputs_only_snapshot(snapshot)
    output = _new_destination(output)
    names = ["manifest.json"]
    names.extend(sorted({f"objects/sha256/{record['sha256']}" for record in manifest["inputs"]}))
    try:
        with tempfile.TemporaryDirectory(dir=output.parent, prefix=".transport-") as temporary:
            candidate = Path(temporary) / "parts"
            candidate.mkdir(mode=0o700)
            listing = Path(temporary) / "files"
            listing.write_text("\n".join(names) + "\n", encoding="ascii")
            command = [
                "tar",
                "--create",
                "--file=-",
                "--directory",
                str(snapshot),
                "--files-from",
                str(listing),
                "--owner=0",
                "--group=0",
                "--numeric-owner",
                "--mtime=@0",
                "--mode=600",
            ]
            with (
                _SplitWriter(candidate, prefix) as writer,
                _zstd_output(command, decompress=False) as stream,
            ):
                shutil.copyfileobj(stream, writer)
            checksums = "".join(f"{sha256_file(path)}  {path.name}\n" for path in writer.paths)
            index = candidate / f"{prefix}.sha256"
            index.write_text(checksums, encoding="ascii")
            index.chmod(0o600)
            candidate.rename(output)
    except (OSError, tarfile.TarError) as error:
        fail(f"dependency transport export failed: {error}")
    return output / f"{prefix}.sha256"


def transport_records(
    directory: Path, *, index_name: str, index_sha256: str
) -> list[tuple[str, str]]:
    if not is_sha256(index_sha256):
        fail("dependency transport index SHA-256 is invalid")
    prefix = index_name.removesuffix(".sha256")
    if not index_name.endswith(".sha256") or _TRANSPORT_PREFIX.fullmatch(prefix) is None:
        fail("dependency transport index must identify one FPLinux release")
    index = directory / index_name
    if index.is_symlink() or not index.is_file() or sha256_file(index) != index_sha256:
        fail("dependency transport checksum index is missing or mismatched")
    try:
        lines = index.read_text(encoding="ascii").splitlines()
    except UnicodeError:
        fail("dependency transport checksum index is invalid")
    if not lines:
        fail("dependency transport checksum index lists no parts")
    records = []
    for position, line in enumerate(lines):
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if match is None or match[2] != f"{prefix}.tar.zst.part-{position:05d}":
            fail("dependency transport checksum index part order is invalid")
        records.append((match[1], match[2]))
    return records


def transport_parts(parts: Path, *, index_name: str, index_sha256: str) -> tuple[str, ...]:
    """Verify the pinned checksum file and list the exact parts a downloader needs."""
    try:
        return tuple(
            name
            for _digest, name in transport_records(
                parts, index_name=index_name, index_sha256=index_sha256
            )
        )
    except OSError as error:
        fail(f"dependency transport index cannot be read: {error}")


def verify_transport(directory: Path, *, index_name: str, index_sha256: str) -> tuple[Path, ...]:
    """Check the exact indexed parts before inspection, restoration or publication."""
    records = transport_records(directory, index_name=index_name, index_sha256=index_sha256)
    parts = []
    for position, (digest, name) in enumerate(records):
        path = directory / name
        if path.is_symlink() or not path.is_file() or sha256_file(path) != digest:
            fail(f"dependency transport part is missing or mismatched: {name}")
        size = path.stat().st_size
        if not 0 < size <= _PART_BYTES or (position < len(records) - 1 and size != _PART_BYTES):
            fail(f"dependency transport part size is invalid: {name}")
        parts.append(path)
    return tuple(parts)


def _extract_snapshot(paths: Sequence[Path], destination: Path) -> None:
    extracted = set()
    command = ["cat", "--", *(str(path) for path in paths)]
    with (
        _zstd_output(command, decompress=True) as stream,
        tarfile.open(fileobj=stream, mode="r|") as archive,
    ):
        for member in archive:
            name = member.name
            if (
                not member.isfile()
                or name in extracted
                or (
                    name != "manifest.json"
                    and re.fullmatch(r"objects/sha256/[0-9a-f]{64}", name) is None
                )
            ):
                fail(f"dependency transport archive member is invalid: {name}")
            source = archive.extractfile(member)
            if source is None:
                fail(f"dependency transport archive file is missing: {name}")
            path = destination / name
            path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            with source, path.open("xb") as output:
                shutil.copyfileobj(source, output)
            path.chmod(0o600)
            extracted.add(name)
    manifest = _inputs_only_snapshot(destination)
    expected = {"manifest.json"}
    expected.update(f"objects/sha256/{record['sha256']}" for record in manifest["inputs"])
    if extracted != expected:
        fail("dependency transport archive contains objects outside its snapshot")


def restore_transport(
    parts: Path, destination: Path, *, index_name: str, index_sha256: str
) -> Path:
    """Verify the pinned index and every part before publishing the original snapshot."""
    try:
        paths = verify_transport(parts, index_name=index_name, index_sha256=index_sha256)
        destination = _new_destination(destination)
        with tempfile.TemporaryDirectory(
            dir=destination.parent, prefix=".transport-"
        ) as temporary:
            candidate = Path(temporary) / "snapshot"
            candidate.mkdir(mode=0o700)
            _extract_snapshot(paths, candidate)
            candidate.rename(destination)
    except (OSError, tarfile.TarError) as error:
        fail(f"dependency transport restore failed: {error}")
    return destination
