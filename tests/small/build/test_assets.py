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
    """Provide independently pinned loader bytes and one common archive."""
    return {
        "fdl1": {
            "url": "https://example.invalid/loader.bin",
            "sha256": "351d3b22c4095342f0ecd8d9b1cb3e7e4d7510cae44df361dfbb101584f51d92",
            "cache_name": "loader.bin",
            "license": "MIT",
            "path": "loader.bin",
        },
        "maps": {
            "url": "https://example.invalid/maps.7z",
            "sha256": "b9cfa94626ae7e693c67d7149ed162959cb2e5f473c77f7fdc99b640f463d587",
            "cache_name": "maps.7z",
            "license": "NOASSERTION",
            "pinmap_path": "pinmap.bin",
            "keymap_path": "keymap.bin",
        },
    }


@pytest.fixture
def loader_maps() -> dict[str, Any]:
    """Select two archive members with their own output pins."""
    return {
        "pinmap": {
            "member": "phone-a/pinmap.bin",
            "sha256": "95576f9cf8c395c85dddf4b280c325f142c85b0fcb61490c944ce8349b9ab191",
        },
        "keymap": {
            "member": "phone-a/keymap.bin",
            "sha256": "013221bc2d755ec6c48b1f21bbae87a9f63cb93a45e5f6fc77209788567f0942",
        },
    }


@pytest.fixture
def cached_downloads(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Replace network transfer with local, valid source files for the host component."""
    cache = tmp_path / "cache"
    downloads = cache / "downloads"
    downloads.mkdir(parents=True)
    (downloads / "loader.bin").write_bytes(b"loader payload")
    (downloads / "maps.7z").write_bytes(b"controlled map archive")
    monkeypatch.setattr(inputs, "CACHE", cache)

    def refuse_network(_url: str, _destination: Path, **_kwargs: object) -> Path:
        pytest.fail("this host component must use its controlled download files")

    monkeypatch.setattr(sources, "transfer_verified_file", refuse_network)


def _write_common_lock(path: Path, declaration: dict[str, Any]) -> None:
    """Write controlled source declarations without using the production serializer."""
    lines = []
    for name, record in declaration.items():
        lines.append(f"[{name}]")
        lines.extend(f"{key} = {json.dumps(value)}" for key, value in record.items())
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


@pytest.mark.usefixtures("cached_downloads")
def test_build_assets_delivers_a_self_contained_loader_and_map_lock(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    common_declaration: dict[str, Any],
    loader_maps: dict[str, Any],
) -> None:
    """The delivered lock retains every source pin and selected archive member."""
    platform = tmp_path / "platform.lock.toml"
    _write_common_lock(platform, common_declaration)
    resolved = load_asset_sources(platform, loader_maps=loader_maps)
    platform.unlink()

    def extract_controlled_member(_archive: Path, member: str) -> bytes:
        """Replace only the external 7z executable with controlled member bytes."""
        return {"phone-a/pinmap.bin": b"board pins", "phone-a/keymap.bin": b"board keys"}[member]

    monkeypatch.setattr(assets, "extract_7z_member", extract_controlled_member)
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
            },
            {
                "id": "maps",
                "kind": "7z",
                "url": "https://example.invalid/maps.7z",
                "sha256": "b9cfa94626ae7e693c67d7149ed162959cb2e5f473c77f7fdc99b640f463d587",
                "cache_name": "maps.7z",
                "license": "NOASSERTION",
                "output": [
                    {
                        "role": "pinmap",
                        "path": "pinmap.bin",
                        "member": "phone-a/pinmap.bin",
                        "sha256": (
                            "95576f9cf8c395c85dddf4b280c325f142c85b0fcb61490c944ce8349b9ab191"
                        ),
                    },
                    {
                        "role": "keymap",
                        "path": "keymap.bin",
                        "member": "phone-a/keymap.bin",
                        "sha256": (
                            "013221bc2d755ec6c48b1f21bbae87a9f63cb93a45e5f6fc77209788567f0942"
                        ),
                    },
                ],
            },
        ]
    }
    assert asset_bundle_paths(resolved) == {
        "fdl1": "assets/loader.bin",
        "pinmap": "assets/pinmap.bin",
        "keymap": "assets/keymap.bin",
    }
    assert result == {
        "fdl1": ("loader.bin", "351d3b22c4095342f0ecd8d9b1cb3e7e4d7510cae44df361dfbb101584f51d92"),
        "pinmap": (
            "pinmap.bin",
            "95576f9cf8c395c85dddf4b280c325f142c85b0fcb61490c944ce8349b9ab191",
        ),
        "keymap": (
            "keymap.bin",
            "013221bc2d755ec6c48b1f21bbae87a9f63cb93a45e5f6fc77209788567f0942",
        ),
    }
    assert (output / "loader.bin").read_bytes() == b"loader payload"
    assert (output / "pinmap.bin").read_bytes() == b"board pins"
    assert (output / "keymap.bin").read_bytes() == b"board keys"
    for path in (effective, output / "loader.bin", output / "pinmap.bin", output / "keymap.bin"):
        assert path.stat().st_mode & 0o777 == 0o600


@pytest.mark.usefixtures("cached_downloads")
def test_build_assets_rejects_source_bytes_even_when_the_member_pins_match(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    common_declaration: dict[str, Any],
    loader_maps: dict[str, Any],
) -> None:
    """Output integrity cannot replace the downloaded archive's integrity check."""
    common_declaration["maps"]["sha256"] = "a" * 64
    resolved = resolve_asset_sources(common_declaration, loader_maps=loader_maps)

    def controlled_download(
        _url: str, destination: Path, *, verify: Callable[[Path], None]
    ) -> Path:
        """Replace HTTPS transfer while retaining production source verification."""
        destination.write_bytes(b"controlled map archive")
        verify(destination)
        return destination

    monkeypatch.setattr(sources, "transfer_verified_file", controlled_download)
    effective = tmp_path / "delivered.lock.toml"
    output = tmp_path / "assets"
    with pytest.raises(SystemExit, match=r"maps\.7z SHA256 mismatch"):
        assets.build_assets(resolved, output, lock_output=effective)
    assert not (output / "pinmap.bin").exists()
    assert not effective.exists()


@pytest.mark.usefixtures("cached_downloads")
def test_build_assets_rejects_member_bytes_even_when_the_source_pin_matches(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    common_declaration: dict[str, Any],
    loader_maps: dict[str, Any],
) -> None:
    """A verified archive still must yield bytes matching the target's member digest."""
    resolved = resolve_asset_sources(common_declaration, loader_maps=loader_maps)
    monkeypatch.setattr(assets, "extract_7z_member", lambda _archive, _member: b"wrong board pins")
    effective = tmp_path / "delivered.lock.toml"
    output = tmp_path / "assets"
    with pytest.raises(SystemExit, match=r"pinmap\.bin SHA256 mismatch"):
        assets.build_assets(resolved, output, lock_output=effective)
    assert not (output / "pinmap.bin").exists()
    assert not effective.exists()


@pytest.mark.usefixtures("cached_downloads")
def test_fdl_only_target_resolves_and_builds_without_archive_outputs(
    tmp_path: Path,
    common_declaration: dict[str, Any],
) -> None:
    """An omitted target map pair keeps only the common loader download and output."""
    resolved = resolve_asset_sources(common_declaration, loader_maps=None)
    assert resolved == [
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
                    "sha256": "351d3b22c4095342f0ecd8d9b1cb3e7e4d7510cae44df361dfbb101584f51d92",
                }
            ],
        }
    ]
    assert asset_bundle_paths(resolved) == {"fdl1": "assets/loader.bin"}
    output = tmp_path / "assets"
    effective = tmp_path / "delivered.lock.toml"
    assets.build_assets(resolved, output, lock_output=effective)
    assert (output / "loader.bin").read_bytes() == b"loader payload"
    assert not (output / "pinmap.bin").exists()
    assert not (output / "keymap.bin").exists()
    delivered = tomllib.loads(effective.read_text(encoding="utf-8"))
    assert [source["id"] for source in delivered["source"]] == ["fdl1"]


@pytest.mark.parametrize("selected", [{}, {"pinmap": {}}, {"keymap": {}}])
def test_loader_maps_requires_a_complete_pair(
    common_declaration: dict[str, Any], selected: dict[str, Any]
) -> None:
    """A partial declaration cannot silently drop either map role."""
    with pytest.raises(SystemExit, match="target loader_maps must contain exactly"):
        resolve_asset_sources(common_declaration, loader_maps=selected)


@pytest.mark.parametrize(
    ("resource", "field", "value", "message"),
    [
        ("fdl1", "url", "http://example.invalid/loader.bin", "url must use HTTPS"),
        ("maps", "sha256", "invalid", "must be a lowercase SHA-256 digest"),
        ("fdl1", "cache_name", "../loader.bin", "normalized relative path"),
        ("maps", "keymap_path", "/keymap.bin", "normalized relative path"),
        ("maps", "pinmap_path", "loader.bin", "output path is duplicated"),
        ("maps", "keymap_path", "pinmap.bin", "output path is duplicated"),
    ],
)
def test_loader_sources_reject_invalid_downloads_and_conflicting_outputs(  # noqa: PLR0913 -- fixtures and distinct parameterized source inputs.
    *,
    common_declaration: dict[str, Any],
    loader_maps: dict[str, Any],
    resource: str,
    field: str,
    value: str,
    message: str,
) -> None:
    """Source pins and destination paths cannot bypass integrity or overwrite another role."""
    common_declaration[resource][field] = value
    with pytest.raises(SystemExit, match=message):
        resolve_asset_sources(common_declaration, loader_maps=loader_maps)


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("member", "../pinmap.bin", "normalized relative path"),
        ("sha256", "A" * 64, "must be a lowercase SHA-256 digest"),
    ],
)
def test_target_map_member_requires_a_relative_path_and_valid_digest(
    common_declaration: dict[str, Any],
    loader_maps: dict[str, Any],
    field: str,
    value: str,
    message: str,
) -> None:
    """A map cannot escape its archive selection or lose its independent output pin."""
    loader_maps["pinmap"][field] = value
    with pytest.raises(SystemExit, match=message):
        resolve_asset_sources(common_declaration, loader_maps=loader_maps)


def test_source_array_is_rejected_by_the_common_loader_schema() -> None:
    """Common loader declarations accept the two resource tables only."""
    with pytest.raises(SystemExit, match="asset lock must contain exactly: fdl1, maps"):
        resolve_asset_sources({"source": []}, loader_maps=None)
