# SPDX-License-Identifier: GPL-2.0-only
"""Filesystem checks for the split dependency snapshot transport."""

from __future__ import annotations

import hashlib
import io
import subprocess
import tarfile
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.dependencies import transport
from fplinux_cli.dependencies.inputs import DependencyInput
from fplinux_cli.dependencies.snapshots import preserve_inputs, publish_snapshot, read_snapshot
from fplinux_cli.dependencies.transport import export_snapshot, restore_transport, transport_parts

if TYPE_CHECKING:
    from pathlib import Path

_ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
_PREFIX = "FPLinux-123456789abc-inputs"
_INDEX_NAME = "FPLinux-123456789abc-inputs.sha256"


@pytest.fixture
def input_snapshot(tmp_path: Path) -> Path:
    """Own a saved original with two consumers and one content-addressed object."""
    cache = tmp_path / "checkout/.cache"
    source = cache / "downloads/original.tar"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"abc")
    inputs = [
        DependencyInput(
            key=key,
            url="https://example.invalid/original.tar",
            sha256=_ABC_SHA256,
            size=3,
            destination="downloads/original.tar",
            purpose="build source",
        )
        for key in ("first", "second")
    ]
    snapshot = tmp_path / "originals"
    manifest = preserve_inputs(
        inputs, {"selection": "example"}, snapshot, cache=cache, offline=True
    )
    publish_snapshot(snapshot, manifest)
    return snapshot


def test_transport_is_standard_tar_zstd_and_restores_the_original_snapshot(
    input_snapshot: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Small parts exercise the same streaming split and join without a giant fixture."""
    monkeypatch.setattr(transport, "_PART_BYTES", 128)
    original_manifest = (input_snapshot / "manifest.json").read_bytes()
    (input_snapshot / "private-unrelated-file").write_bytes(b"never preserved")

    index = export_snapshot(input_snapshot, tmp_path / "parts", prefix=_PREFIX)

    lines = index.read_text().splitlines()
    assert len(lines) > 1
    joined = bytearray()
    for position, line in enumerate(lines):
        digest, name = line.split("  ")
        assert name == f"FPLinux-123456789abc-inputs.tar.zst.part-{position:05d}"
        data = (index.parent / name).read_bytes()
        assert hashlib.sha256(data).hexdigest() == digest
        assert 0 < len(data) <= 128
        if position < len(lines) - 1:
            assert len(data) == 128
        joined.extend(data)
    decoded = subprocess.run(
        ["zstd", "--decompress", "--stdout", "--quiet"],
        input=bytes(joined),
        capture_output=True,
        check=True,
    )
    with tarfile.open(fileobj=io.BytesIO(decoded.stdout)) as archive:
        assert archive.getnames() == ["manifest.json", f"objects/sha256/{_ABC_SHA256}"]
        assert all((member.uid, member.gid, member.mtime) == (0, 0, 0) for member in archive)
        source = archive.extractfile(f"objects/sha256/{_ABC_SHA256}")
        assert source is not None
        assert source.read() == b"abc"
    (index.parent / "FPLinux-123456789abc-inputs.tar.zst.part-99999").write_bytes(
        b"unlisted bytes"
    )
    pinned_hash = hashlib.sha256(index.read_bytes()).hexdigest()

    restored = restore_transport(
        index.parent, tmp_path / "restored", index_name=_INDEX_NAME, index_sha256=pinned_hash
    )

    assert restored == tmp_path / "restored"
    assert (restored / "manifest.json").read_bytes() == original_manifest
    assert (restored / f"objects/sha256/{_ABC_SHA256}").read_bytes() == b"abc"
    assert not (restored / "private-unrelated-file").exists()
    assert len(read_snapshot(restored)["inputs"]) == 2


@pytest.mark.parametrize("damage", ["missing", "modified"])
def test_missing_or_modified_listed_part_prevents_snapshot_publication(
    input_snapshot: Path, tmp_path: Path, damage: str
) -> None:
    """Integrity failures stop before any destination snapshot becomes available."""
    index = export_snapshot(input_snapshot, tmp_path / "parts", prefix=_PREFIX)
    pinned_hash = hashlib.sha256(index.read_bytes()).hexdigest()
    part = index.parent / "FPLinux-123456789abc-inputs.tar.zst.part-00000"
    if damage == "missing":
        part.unlink()
    else:
        part.write_bytes(b"modified transport")

    with pytest.raises(SystemExit, match="part is missing or mismatched"):
        restore_transport(
            index.parent, tmp_path / "restored", index_name=_INDEX_NAME, index_sha256=pinned_hash
        )

    assert not (tmp_path / "restored").exists()


def test_index_mismatch_is_rejected_before_reading_its_filenames(
    input_snapshot: Path, tmp_path: Path
) -> None:
    """The independently pinned checksum file is the first transport trust boundary."""
    index = export_snapshot(input_snapshot, tmp_path / "parts", prefix=_PREFIX)
    pinned_hash = hashlib.sha256(index.read_bytes()).hexdigest()
    index.write_text("untrusted index\n")

    with pytest.raises(SystemExit, match="checksum index is missing or mismatched"):
        restore_transport(
            index.parent, tmp_path / "restored", index_name=_INDEX_NAME, index_sha256=pinned_hash
        )

    assert not (tmp_path / "restored").exists()


def test_pinned_index_lists_parts_before_a_downloader_has_fetched_them(tmp_path: Path) -> None:
    """A downloader obtains names only from the verified standard checksum file."""
    index = tmp_path / _INDEX_NAME
    checksum_text = (
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "  FPLinux-123456789abc-inputs.tar.zst.part-00000\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "  FPLinux-123456789abc-inputs.tar.zst.part-00001\n"
    )
    index.write_text(checksum_text)
    pinned_hash = hashlib.sha256(index.read_bytes()).hexdigest()

    assert transport_parts(tmp_path, index_name=_INDEX_NAME, index_sha256=pinned_hash) == (
        "FPLinux-123456789abc-inputs.tar.zst.part-00000",
        "FPLinux-123456789abc-inputs.tar.zst.part-00001",
    )


def test_reordered_index_cannot_change_the_archive_part_order(
    input_snapshot: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A verified index must list every sequential part in its transport order."""
    monkeypatch.setattr(transport, "_PART_BYTES", 128)
    index = export_snapshot(input_snapshot, tmp_path / "parts", prefix=_PREFIX)
    lines = index.read_text().splitlines()
    assert len(lines) > 1
    index.write_text("\n".join(reversed(lines)) + "\n")
    pinned_hash = hashlib.sha256(index.read_bytes()).hexdigest()

    with pytest.raises(SystemExit, match="part order is invalid"):
        restore_transport(
            index.parent, tmp_path / "restored", index_name=_INDEX_NAME, index_sha256=pinned_hash
        )

    assert not (tmp_path / "restored").exists()


def test_missing_snapshot_original_prevents_transport_publication(
    input_snapshot: Path, tmp_path: Path
) -> None:
    """An incomplete saved closure cannot be packaged as a valid transport."""
    (input_snapshot / f"objects/sha256/{_ABC_SHA256}").unlink()

    with pytest.raises(SystemExit, match="object is missing or mismatched"):
        export_snapshot(input_snapshot, tmp_path / "parts", prefix=_PREFIX)

    assert not (tmp_path / "parts").exists()


def test_native_snapshot_verification_rejects_corrupt_archived_originals(
    input_snapshot: Path, tmp_path: Path
) -> None:
    """Valid transport checksums cannot replace the snapshot's original object checksums."""
    parts = tmp_path / "parts"
    parts.mkdir()
    part = parts / "FPLinux-123456789abc-inputs.tar.zst.part-00000"
    files = {
        "manifest.json": (input_snapshot / "manifest.json").read_bytes(),
        f"objects/sha256/{_ABC_SHA256}": b"bad",
    }
    plain_tar = io.BytesIO()
    with tarfile.open(fileobj=plain_tar, mode="w") as archive:
        for name, data in files.items():
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    with part.open("wb") as output:
        subprocess.run(
            ["zstd", "--compress", "--stdout", "--quiet"],
            input=plain_tar.getvalue(),
            stdout=output,
            check=True,
        )
    index = parts / _INDEX_NAME
    index.write_text(f"{hashlib.sha256(part.read_bytes()).hexdigest()}  {part.name}\n")
    pinned_hash = hashlib.sha256(index.read_bytes()).hexdigest()

    with pytest.raises(SystemExit, match="object is missing or mismatched"):
        restore_transport(
            parts, tmp_path / "restored", index_name=_INDEX_NAME, index_sha256=pinned_hash
        )

    assert not (tmp_path / "restored").exists()


def test_zstd_failure_reports_the_codec_cause_without_publishing_a_snapshot(
    tmp_path: Path,
) -> None:
    """A verified part with invalid compressed bytes preserves the native codec failure."""
    parts = tmp_path / "parts"
    parts.mkdir()
    part = parts / "FPLinux-123456789abc-inputs.tar.zst.part-00000"
    part.write_bytes(b"not a zstd frame")
    index = parts / _INDEX_NAME
    index.write_text(f"{hashlib.sha256(part.read_bytes()).hexdigest()}  {part.name}\n")
    pinned_hash = hashlib.sha256(index.read_bytes()).hexdigest()

    with pytest.raises(SystemExit, match=r"dependency transport command failed:.*zstd exit 1"):
        restore_transport(
            parts, tmp_path / "restored", index_name=_INDEX_NAME, index_sha256=pinned_hash
        )

    assert not (tmp_path / "restored").exists()


def test_existing_transport_destination_is_preserved(input_snapshot: Path, tmp_path: Path) -> None:
    """Immutable output names cannot silently replace an earlier transport."""
    output = tmp_path / "parts"
    output.mkdir()
    existing = output / "keep"
    existing.write_bytes(b"previous saved bytes")

    with pytest.raises(SystemExit, match="destination already exists"):
        export_snapshot(input_snapshot, output, prefix=_PREFIX)

    assert existing.read_bytes() == b"previous saved bytes"
