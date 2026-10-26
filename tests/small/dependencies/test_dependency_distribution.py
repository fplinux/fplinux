# SPDX-License-Identifier: GPL-2.0-only
"""Release metadata binds its source context to an authenticated input transport."""

from __future__ import annotations

import hashlib
import json
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.artifacts.release import ReleaseContext
from fplinux_cli.dependencies.distribution import (
    dependency_metadata,
    read_dependency_distribution,
    write_dependency_distribution,
)

if TYPE_CHECKING:
    from pathlib import Path

_RELEASE = ReleaseContext("123456789abc", "123456789abc" + "0" * 28, "a" * 64)


def test_release_metadata_authenticates_the_index_and_exact_part_bytes(tmp_path: Path) -> None:
    part = tmp_path / "FPLinux-123456789abc-inputs.tar.zst.part-00000"
    part.write_bytes(b"abc")
    index = tmp_path / "FPLinux-123456789abc-inputs.sha256"
    index.write_bytes(
        b"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        b"  FPLinux-123456789abc-inputs.tar.zst.part-00000\n"
    )

    saved = write_dependency_distribution(
        tmp_path, release=_RELEASE, snapshot="b" * 64, index=index
    )
    restored = read_dependency_distribution(tmp_path)

    assert restored == saved
    assert restored.release == _RELEASE
    assert restored.snapshot == "b" * 64
    assert restored.files == (
        "DEPENDENCIES.json",
        "FPLinux-123456789abc-inputs.sha256",
        "FPLinux-123456789abc-inputs.tar.zst.part-00000",
    )
    assert json.loads((tmp_path / "DEPENDENCIES.json").read_bytes()) == {
        "release_id": "123456789abc",
        "source_commit": "123456789abc" + "0" * 28,
        "source_snapshot": "a" * 64,
        "snapshot": "b" * 64,
        "index": "FPLinux-123456789abc-inputs.sha256",
        "index_sha256": hashlib.sha256(index.read_bytes()).hexdigest(),
    }
    part.write_bytes(b"changed")
    with pytest.raises(SystemExit, match="part is missing or mismatched"):
        read_dependency_distribution(tmp_path)


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("release_id", "abcdef123456", "does not match its source identity"),
        ("index", "FPLinux-abcdef123456-inputs.sha256", "differs from its release ID"),
        ("index_sha256", "latest", "index SHA-256 is invalid"),
        ("snapshot", "unknown", "snapshot ID is invalid"),
    ],
)
def test_download_metadata_rejects_unpinned_or_mismatched_identity(
    field: str, value: str, message: str
) -> None:
    metadata = {
        "release_id": "123456789abc",
        "source_commit": "123456789abc" + "0" * 28,
        "source_snapshot": "a" * 64,
        "snapshot": "b" * 64,
        "index": "FPLinux-123456789abc-inputs.sha256",
        "index_sha256": "c" * 64,
    }
    metadata[field] = value

    with pytest.raises(SystemExit, match=message):
        dependency_metadata(json.dumps(metadata).encode())
