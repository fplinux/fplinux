# SPDX-License-Identifier: GPL-2.0-only
"""Resolved loader originals and the self-contained lock delivered with their bytes."""

from __future__ import annotations

import tomllib
from typing import TYPE_CHECKING

import pytest
from fplinux_cli import common
from fplinux_cli.build import assets, inputs
from fplinux_cli.manifests.assets import (
    asset_bundle_paths,
    load_asset_sources,
    resolve_asset_sources,
)

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def loader_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> tuple[Path, Path]:
    """Own one pinned loader original in its canonical cache location."""
    root = tmp_path / "checkout"
    root.mkdir()
    (root / "sources.lock.toml").write_text(
        '[loader]\nversion="1"\nname="loader.bin"\n'
        'url="https://example.invalid/loader.bin"\nalgorithm="sha256"\n'
        'checksum="351d3b22c4095342f0ecd8d9b1cb3e7e4d7510cae44df361dfbb101584f51d92"\n'
        'license="MIT"\n'
    )
    lock = root / "assets.lock.toml"
    lock.write_text('[fdl1]\nsource="loader"\npath="loader.bin"\n')
    cache = root / ".cache"
    source = cache / "downloads/sources/loader/loader.bin"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"loader payload")
    monkeypatch.setattr(common, "ROOT", root)
    monkeypatch.setattr(inputs, "CACHE", cache)
    return root, lock


def test_build_assets_delivers_local_bytes_and_a_self_contained_lock(
    loader_checkout: tuple[Path, Path],
    tmp_path: Path,
) -> None:
    root, lock = loader_checkout
    resolved = load_asset_sources(lock, source_root=root)
    lock.unlink()
    effective = tmp_path / "delivered.lock.toml"
    output = tmp_path / "assets"
    result = assets.build_assets(resolved, output, lock_output=effective)
    assert tomllib.loads(effective.read_text()) == {
        "source": [
            {
                "id": "fdl1",
                "kind": "file",
                "url": "https://example.invalid/loader.bin",
                "sha256": "351d3b22c4095342f0ecd8d9b1cb3e7e4d7510cae44df361dfbb101584f51d92",
                "cache_name": "sources/loader/loader.bin",
                "license": "MIT",
                "output": [
                    {
                        "role": "fdl1",
                        "path": "loader.bin",
                        "sha256": (
                            "351d3b22c4095342f0ecd8d9b1cb3e7e4d7510cae44df361dfbb101584f51d92"
                        ),
                    }
                ],
            }
        ],
    }
    assert asset_bundle_paths(resolved) == {"fdl1": "assets/loader.bin"}
    assert result == {
        "fdl1": ("loader.bin", "351d3b22c4095342f0ecd8d9b1cb3e7e4d7510cae44df361dfbb101584f51d92"),
    }
    assert (output / "loader.bin").read_bytes() == b"loader payload"
    assert effective.stat().st_mode & 0o777 == 0o600
    assert (output / "loader.bin").stat().st_mode & 0o777 == 0o600


def test_mismatched_loader_bytes_cannot_publish_an_output_or_lock(
    loader_checkout: tuple[Path, Path],
    tmp_path: Path,
) -> None:
    root, lock = loader_checkout
    resolved = load_asset_sources(lock, source_root=root)
    (root / ".cache/downloads/sources/loader/loader.bin").write_bytes(b"wrong payload")
    effective = tmp_path / "delivered.lock.toml"
    output = tmp_path / "assets"
    with pytest.raises(SystemExit, match="offline locked input is missing or mismatched"):
        assets.build_assets(resolved, output, lock_output=effective)
    assert not (output / "loader.bin").exists()
    assert not effective.exists()


@pytest.mark.parametrize(
    ("replacement", "message"),
    [
        ('source="missing"\npath="loader.bin"', "source lock entry is missing"),
        ('source="loader"\npath="/loader.bin"', "normalized relative path"),
        ('source="loader"\npath="../loader.bin"', "normalized relative path"),
    ],
)
def test_loader_reference_rejects_missing_owners_and_escaping_outputs(
    loader_checkout: tuple[Path, Path],
    replacement: str,
    message: str,
) -> None:
    root, lock = loader_checkout
    lock.write_text("[fdl1]\n" + replacement + "\n")
    with pytest.raises(SystemExit, match=message):
        load_asset_sources(lock, source_root=root)


def test_source_array_is_rejected_by_the_platform_loader_schema() -> None:
    with pytest.raises(SystemExit, match="asset lock must contain exactly: fdl1"):
        resolve_asset_sources({"source": []})
