# SPDX-License-Identifier: GPL-2.0-only
"""Prepare and restore one workflow artifact through its real local snapshot boundary."""

from __future__ import annotations

import io
import urllib.request
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.ci.inputs import prepare_inputs, restore_inputs
from fplinux_cli.dependencies.distribution import read_dependency_distribution

from tests.small.dependencies.release_fixtures import make_release_checkout

if TYPE_CHECKING:
    from pathlib import Path

    from tests.small.dependencies.release_fixtures import ReleaseCheckout


@pytest.fixture
def release_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> ReleaseCheckout:
    return make_release_checkout(tmp_path, monkeypatch)


def test_prepare_exports_exact_originals_once_and_passes_snapshot_to_following_jobs(
    release_checkout: ReleaseCheckout, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Controlled HTTP originals exercise selection, native hashes and real compression."""
    requested: list[str] = []

    def original(request: urllib.request.Request, *, timeout: int) -> io.BytesIO:
        assert timeout == 60
        assert request.full_url.startswith("https://example.invalid/")
        requested.append(request.full_url)
        return io.BytesIO(b"abc")

    monkeypatch.setattr(urllib.request, "urlopen", original)
    output = tmp_path / "parts"
    workflow_output = tmp_path / "workflow-output"
    monkeypatch.setenv("GITHUB_OUTPUT", str(workflow_output))

    prepare_inputs(output)

    distribution = read_dependency_distribution(output)
    assert distribution.release.source_commit == release_checkout.commit
    assert distribution.release.release_id == release_checkout.release_id
    assert "https://example.invalid/support.tar.gz" in requested
    assert "https://example.invalid/main/armv7/runtime-1-r0.apk" in requested
    assert len(requested) == len(set(requested))
    assert workflow_output.read_text() == (
        f"snapshot={distribution.snapshot}\nrelease_id={release_checkout.release_id}\n"
    )
    assert not (release_checkout.root / ".cache/environment").exists()


def test_local_workflow_artifact_restores_native_originals_without_network(
    release_checkout: ReleaseCheckout, tmp_path: Path
) -> None:
    parts, snapshot = release_checkout.preserve(tmp_path / "parts")

    restore_inputs(parts, snapshot=snapshot)

    release_checkout.assert_originals_restored()


def test_another_workflow_snapshot_is_rejected_before_restoration(
    release_checkout: ReleaseCheckout, tmp_path: Path
) -> None:
    parts, _snapshot = release_checkout.preserve(tmp_path / "parts")

    with pytest.raises(SystemExit, match="differs from the prepared snapshot"):
        restore_inputs(parts, snapshot="a" * 64)

    assert not (release_checkout.root / ".cache/downloads").exists()


def test_changed_checkout_rejects_the_artifact_before_restoration(
    release_checkout: ReleaseCheckout, tmp_path: Path
) -> None:
    parts, snapshot = release_checkout.preserve(tmp_path / "parts")
    source = release_checkout.root / "sources.lock.toml"
    source.write_text(source.read_text().replace('version = "6.0"', 'version = "6.1"'))

    with pytest.raises(SystemExit, match="belongs to another source release"):
        restore_inputs(parts, snapshot=snapshot)

    assert not (release_checkout.root / ".cache/downloads").exists()
