# SPDX-License-Identifier: GPL-2.0-only
"""Restore public release inputs with real local transport and controlled HTTP bytes."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.dependencies.releases import restore_release

from .release_fixtures import make_release_checkout, serve_release_bytes

if TYPE_CHECKING:
    from pathlib import Path

    from .release_fixtures import ReleaseCheckout


@pytest.fixture
def release_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> ReleaseCheckout:
    return make_release_checkout(tmp_path, monkeypatch)


def test_explicit_matching_public_release_restores_original_native_cache_slots(
    release_checkout: ReleaseCheckout, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    parts, _snapshot = release_checkout.preserve(tmp_path / "parts")
    requested = serve_release_bytes(monkeypatch, parts, release_checkout.release_id)

    restore_release(release_checkout.release_id)

    release_checkout.assert_originals_restored()
    assert requested == [
        "DEPENDENCIES.json",
        f"FPLinux-{release_checkout.release_id}-inputs.sha256",
        f"FPLinux-{release_checkout.release_id}-inputs.tar.zst.part-00000",
    ]


def test_metadata_for_another_source_stops_before_download_of_its_index(
    release_checkout: ReleaseCheckout, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    parts, _snapshot = release_checkout.preserve(tmp_path / "parts")
    path = parts / "DEPENDENCIES.json"
    metadata = json.loads(path.read_bytes())
    metadata["source_snapshot"] = "a" * 64
    path.write_text(json.dumps(metadata))
    requested = serve_release_bytes(monkeypatch, parts, release_checkout.release_id)

    with pytest.raises(SystemExit, match="belongs to another source release"):
        restore_release(release_checkout.release_id)

    assert requested == ["DEPENDENCIES.json"]
    assert not (release_checkout.root / ".data/downloads").exists()


@pytest.mark.parametrize("damage", ["index", "part"])
def test_release_checksum_failure_never_restores_originals(
    release_checkout: ReleaseCheckout,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    damage: str,
) -> None:
    parts, _snapshot = release_checkout.preserve(tmp_path / "parts")
    name = f"FPLinux-{release_checkout.release_id}-inputs"
    damaged = name + (".sha256" if damage == "index" else ".tar.zst.part-00000")
    (parts / damaged).write_bytes(b"different downloaded bytes")
    requested = serve_release_bytes(monkeypatch, parts, release_checkout.release_id)

    with pytest.raises(SystemExit, match="SHA256 mismatch"):
        restore_release(release_checkout.release_id)

    assert requested[-1] == damaged
    assert len(requested) == (2 if damage == "index" else 3)
    assert not (release_checkout.root / ".data/downloads").exists()


@pytest.mark.usefixtures("release_checkout")
def test_other_explicit_release_is_rejected_before_network_access(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    requested = serve_release_bytes(monkeypatch, tmp_path, "000000000000")

    with pytest.raises(SystemExit, match="does not match this source checkout"):
        restore_release("000000000000")

    assert requested == []
