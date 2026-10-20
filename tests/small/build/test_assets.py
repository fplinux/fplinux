# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for resolved loader assets and their delivered lock."""

from __future__ import annotations

import json
import tomllib
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build import assets, inputs, sources
from fplinux_cli.manifests.assets import (
    asset_bundle_paths,
    load_asset_sources,
    resolve_asset_sources,
)

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path


@pytest.fixture
def common_declaration() -> dict[str, Any]:
    """Provide independently pinned first-stage loader bytes."""
    return {
        "fdl1": {
            "url": "https://example.invalid/loader.bin",
            "sha256": "351d3b22c4095342f0ecd8d9b1cb3e7e4d7510cae44df361dfbb101584f51d92",
            "cache_name": "loader.bin",
            "license": "MIT",
            "path": "loader.bin",
        }
    }


@pytest.fixture
def cached_downloads(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Replace network transfer with a local valid source file for the host component."""
    cache = tmp_path / "cache"
    downloads = cache / "downloads"
    downloads.mkdir(parents=True)
    (downloads / "loader.bin").write_bytes(b"loader payload")
    monkeypatch.setattr(inputs, "CACHE", cache)

    def refuse_network(_url: str, _destination: Path, **_kwargs: object) -> Path:
        pytest.fail("this host component must use its controlled download file")

    monkeypatch.setattr(sources, "transfer_verified_file", refuse_network)


def _write_common_lock(path: Path, declaration: dict[str, Any]) -> None:
    """Write controlled source declarations without using the production serializer."""
    lines = []
    for name, record in declaration.items():
        lines.append(f"[{name}]")
        lines.extend(f"{key} = {json.dumps(value)}" for key, value in record.items())
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


@pytest.mark.usefixtures("cached_downloads")
def test_build_assets_delivers_a_self_contained_loader_lock(
    tmp_path: Path,
    common_declaration: dict[str, Any],
) -> None:
    """A verified cached loader produces its bytes and a complete standalone source pin."""
    platform = tmp_path / "platform.lock.toml"
    _write_common_lock(platform, common_declaration)
    resolved = load_asset_sources(platform)
    platform.unlink()
    effective = tmp_path / "delivered.lock.toml"
    output = tmp_path / "assets"
    result = assets.build_assets(resolved, output, lock_output=effective)

    delivered = tomllib.loads(effective.read_text(encoding="utf-8"))
    assert delivered == {
        "source": [
            {
                "id": "fdl1",
                "kind": "file",
                "url": "https://example.invalid/loader.bin",
                "sha256": "351d3b22c4095342f0ecd8d9b1cb3e7e4d7510cae44df361dfbb101584f51d92",
                "cache_name": "loader.bin",
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
        ]
    }
    assert asset_bundle_paths(resolved) == {"fdl1": "assets/loader.bin"}
    assert result == {
        "fdl1": ("loader.bin", "351d3b22c4095342f0ecd8d9b1cb3e7e4d7510cae44df361dfbb101584f51d92")
    }
    assert (output / "loader.bin").read_bytes() == b"loader payload"
    for path in (effective, output / "loader.bin"):
        assert path.stat().st_mode & 0o777 == 0o600


@pytest.mark.usefixtures("cached_downloads")
def test_build_assets_rejects_source_bytes_that_do_not_match_the_pin(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    common_declaration: dict[str, Any],
) -> None:
    """A failed source verification cannot publish loader bytes or their delivered lock."""
    common_declaration["fdl1"]["sha256"] = "a" * 64
    resolved = resolve_asset_sources(common_declaration)

    def controlled_download(
        _url: str, destination: Path, *, verify: Callable[[Path], None]
    ) -> Path:
        """Replace HTTPS transfer while retaining production source verification."""
        destination.write_bytes(b"wrong loader payload")
        verify(destination)
        return destination

    monkeypatch.setattr(sources, "transfer_verified_file", controlled_download)
    effective = tmp_path / "delivered.lock.toml"
    output = tmp_path / "assets"
    with pytest.raises(SystemExit, match=r"loader\.bin SHA256 mismatch"):
        assets.build_assets(resolved, output, lock_output=effective)
    assert not (output / "loader.bin").exists()
    assert not effective.exists()


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("url", "http://example.invalid/loader.bin", "url must use HTTPS"),
        ("sha256", "invalid", "must be a lowercase SHA-256 digest"),
        ("cache_name", "../loader.bin", "normalized relative path"),
        ("path", "/loader.bin", "normalized relative path"),
        ("license", "", "must be a non-empty string"),
    ],
)
def test_loader_source_rejects_invalid_downloads_and_output_paths(
    common_declaration: dict[str, Any],
    field: str,
    value: str,
    message: str,
) -> None:
    """Source pins and destination paths retain integrity and destination boundaries."""
    common_declaration["fdl1"][field] = value
    with pytest.raises(SystemExit, match=message):
        resolve_asset_sources(common_declaration)


def test_source_array_is_rejected_by_the_common_loader_schema() -> None:
    """The platform lock accepts its first-stage loader declaration only."""
    with pytest.raises(SystemExit, match="asset lock must contain exactly: fdl1"):
        resolve_asset_sources({"source": []})
