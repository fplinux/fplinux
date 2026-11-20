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


def test_public_stable_tag_restores_the_exact_release_inputs(
    release_checkout: ReleaseCheckout, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    parts, _snapshot = release_checkout.preserve(tmp_path / "parts")
    requested = serve_release_bytes(monkeypatch, {"v1.0.0": parts})

    restore_release(release_checkout.release_id)

    release_checkout.assert_originals_restored()
    assert requested == [
        "v1.0.0/DEPENDENCIES.json",
        f"v1.0.0/FPLinux-{release_checkout.release_id}-inputs.sha256",
        f"v1.0.0/FPLinux-{release_checkout.release_id}-inputs.tar.zst.part-00000",
    ]


@pytest.mark.parametrize("stable_state", ["missing", "another-source"])
def test_prerelease_inputs_are_found_when_the_stable_tag_lacks_this_build(
    release_checkout: ReleaseCheckout,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    stable_state: str,
) -> None:
    parts, _snapshot = release_checkout.preserve(tmp_path / "parts")
    tag = f"build-{release_checkout.release_id}"
    releases = {tag: parts}
    if stable_state == "another-source":
        stable = tmp_path / "stable"
        stable.mkdir()
        metadata = json.loads((parts / "DEPENDENCIES.json").read_bytes())
        metadata["source_snapshot"] = "a" * 64
        (stable / "DEPENDENCIES.json").write_text(json.dumps(metadata))
        releases["v1.0.0"] = stable
    requested = serve_release_bytes(monkeypatch, releases)

    restore_release(release_checkout.release_id)

    release_checkout.assert_originals_restored()
    assert requested == [
        "v1.0.0/DEPENDENCIES.json",
        f"{tag}/DEPENDENCIES.json",
        f"{tag}/FPLinux-{release_checkout.release_id}-inputs.sha256",
        f"{tag}/FPLinux-{release_checkout.release_id}-inputs.tar.zst.part-00000",
    ]


def test_metadata_for_another_source_stops_before_download_of_its_index(
    release_checkout: ReleaseCheckout, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    parts, _snapshot = release_checkout.preserve(tmp_path / "parts")
    path = parts / "DEPENDENCIES.json"
    metadata = json.loads(path.read_bytes())
    metadata["source_snapshot"] = "a" * 64
    path.write_text(json.dumps(metadata))
    requested = serve_release_bytes(monkeypatch, {"v1.0.0": parts})

    with pytest.raises(SystemExit, match="belongs to another source release"):
        restore_release(release_checkout.release_id)

    assert requested == [
        "v1.0.0/DEPENDENCIES.json",
        f"build-{release_checkout.release_id}/DEPENDENCIES.json",
    ]
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
    requested = serve_release_bytes(monkeypatch, {"v1.0.0": parts})

    with pytest.raises(SystemExit, match="SHA256 mismatch"):
        restore_release(release_checkout.release_id)

    assert requested[-1] == f"v1.0.0/{damaged}"
    assert len(requested) == (2 if damage == "index" else 3)
    assert not (release_checkout.root / ".data/downloads").exists()


@pytest.mark.usefixtures("release_checkout")
def test_other_explicit_release_is_rejected_before_network_access(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    requested = serve_release_bytes(monkeypatch, {})

    with pytest.raises(SystemExit, match="does not match this source checkout"):
        restore_release("000000000000")

    assert requested == []
