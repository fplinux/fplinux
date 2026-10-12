# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for resolved loader assets and their delivered lock."""

from __future__ import annotations

import hashlib
import json
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build import assets, inputs, sources
from fplinux_cli.manifests.assets import asset_bundle_paths, load_asset_locks

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path


def _write_lock(path: Path, records: list[dict[str, Any]]) -> None:
    """Write controlled source and output declarations for one owner."""
    lines = [] if records else ["source = []"]
    for record in records:
        lines.append("[[source]]")
        for key, value in record.items():
            if key != "output":
                lines.append(f"{key} = {json.dumps(value)}")
        for output in record["output"]:
            lines.append("[[source.output]]")
            lines.extend(f"{key} = {json.dumps(value)}" for key, value in output.items())
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _file_source(source_id: str, role: str, relative: str, payload: bytes) -> dict[str, Any]:
    """Declare a pinned file with separately checked source and output digests."""
    digest = hashlib.sha256(payload).hexdigest()
    return {
        "id": source_id,
        "kind": "file",
        "url": f"https://example.invalid/{source_id}.bin",
        "sha256": digest,
        "cache_name": f"{source_id}.bin",
        "license": "MIT",
        "output": [{"role": role, "path": relative, "sha256": digest}],
    }


def test_build_assets_delivers_a_self_contained_platform_and_board_lock(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The delivered lock retains every pin and board-specific archive member."""
    pin = _file_source("loader", "fdl1", "loader.bin", b"loader payload")
    pins = b"board pins"
    keys = b"board keys"
    maps = {
        "id": "maps",
        "kind": "7z",
        "url": "https://example.invalid/maps.7z",
        "sha256": hashlib.sha256(b"controlled map archive").hexdigest(),
        "cache_name": "maps.7z",
        "license": "NOASSERTION",
        "output": [
            {
                "role": "pinmap",
                "path": "pinmap.bin",
                "member": "phone-a/pinmap.bin",
                "sha256": hashlib.sha256(pins).hexdigest(),
            },
            {
                "role": "keymap",
                "path": "keymap.bin",
                "member": "phone-a/keymap.bin",
                "sha256": hashlib.sha256(keys).hexdigest(),
            },
        ],
    }
    platform = tmp_path / "platform.lock.toml"
    board = tmp_path / "board.lock.toml"
    _write_lock(platform, [pin])
    _write_lock(board, [maps])
    cache = tmp_path / "cache"
    downloads = cache / "downloads"
    downloads.mkdir(parents=True)
    (downloads / "loader.bin").write_bytes(b"loader payload")
    (downloads / "maps.7z").write_bytes(b"controlled map archive")
    monkeypatch.setattr(inputs, "CACHE", cache)

    def extract_controlled_member(_archive: Path, member: str) -> bytes:
        """Replace only the external 7z executable with controlled member bytes."""
        return {"phone-a/pinmap.bin": pins, "phone-a/keymap.bin": keys}[member]

    monkeypatch.setattr(assets, "extract_7z_member", extract_controlled_member)
    effective = tmp_path / "delivered.lock.toml"
    output = tmp_path / "assets"
    result = assets.build_assets((platform, board), output, lock_output=effective)
    platform.unlink()
    board.unlink()

    assert load_asset_locks((effective,)) == [pin, maps]
    assert asset_bundle_paths((effective,)) == {
        "fdl1": "assets/loader.bin",
        "pinmap": "assets/pinmap.bin",
        "keymap": "assets/keymap.bin",
    }
    assert set(result) == {"fdl1", "pinmap", "keymap"}
    assert (output / "loader.bin").read_bytes() == b"loader payload"
    assert (output / "pinmap.bin").read_bytes() == pins
    assert (output / "keymap.bin").read_bytes() == keys
    assert effective.stat().st_mode & 0o777 == 0o600


def test_build_assets_rejects_source_bytes_even_when_the_output_pin_matches(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Output integrity cannot replace the declared downloaded-source integrity check."""
    record = _file_source("loader", "fdl1", "loader.bin", b"loader payload")
    record["sha256"] = "a" * 64
    lock = tmp_path / "source.lock.toml"
    _write_lock(lock, [record])
    monkeypatch.setattr(inputs, "CACHE", tmp_path / "cache")

    def controlled_download(
        _url: str, destination: Path, *, verify: Callable[[Path], None]
    ) -> Path:
        """Replace HTTPS transfer while retaining production source verification."""
        destination.write_bytes(b"loader payload")
        verify(destination)
        return destination

    monkeypatch.setattr(sources, "transfer_verified_file", controlled_download)
    effective = tmp_path / "delivered.lock.toml"
    output = tmp_path / "assets"
    with pytest.raises(SystemExit, match=r"loader\.bin SHA256 mismatch"):
        assets.build_assets((lock,), output, lock_output=effective)
    assert not (output / "loader.bin").exists()
    assert not effective.exists()


def test_build_assets_rejects_output_bytes_even_when_the_source_pin_matches(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A verified downloaded file still must match its declared delivered-output digest."""
    record = _file_source("loader", "fdl1", "loader.bin", b"loader payload")
    record["output"][0]["sha256"] = "b" * 64
    lock = tmp_path / "source.lock.toml"
    _write_lock(lock, [record])
    cache = tmp_path / "cache"
    (cache / "downloads").mkdir(parents=True)
    (cache / "downloads/loader.bin").write_bytes(b"loader payload")
    monkeypatch.setattr(inputs, "CACHE", cache)
    effective = tmp_path / "delivered.lock.toml"
    output = tmp_path / "assets"
    with pytest.raises(SystemExit, match=r"loader\.bin SHA256 mismatch"):
        assets.build_assets((lock,), output, lock_output=effective)
    assert not (output / "loader.bin").exists()
    assert not effective.exists()


@pytest.mark.parametrize(
    ("source_id", "role", "relative", "message"),
    [
        ("loader", "other", "other.bin", "id must be unique"),
        ("other", "fdl1", "other.bin", "role must be unique"),
        ("other", "other", "loader.bin", "output path is duplicated"),
    ],
)
def test_resolved_asset_locks_reject_ambiguous_ownership(
    tmp_path: Path, source_id: str, role: str, relative: str, message: str
) -> None:
    """Neither source identity, runtime role nor bundle path can be silently replaced."""
    platform = tmp_path / "platform.lock.toml"
    board = tmp_path / "board.lock.toml"
    _write_lock(platform, [_file_source("loader", "fdl1", "loader.bin", b"loader")])
    _write_lock(board, [_file_source(source_id, role, relative, b"board")])
    with pytest.raises(SystemExit, match=message):
        load_asset_locks((platform, board))


def test_empty_board_asset_set_keeps_the_platform_loader(tmp_path: Path) -> None:
    """A phone without downloadable board maps uses only its platform loader pin."""
    platform = tmp_path / "platform.lock.toml"
    board = tmp_path / "board.lock.toml"
    pin = _file_source("loader", "fdl1", "loader.bin", b"loader")
    _write_lock(platform, [pin])
    _write_lock(board, [])
    assert load_asset_locks((platform, board)) == [pin]
    assert asset_bundle_paths((platform, board)) == {"fdl1": "assets/loader.bin"}
