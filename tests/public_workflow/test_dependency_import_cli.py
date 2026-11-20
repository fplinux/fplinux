# SPDX-License-Identifier: GPL-2.0-only
"""Import release dependency parts through the public CLI."""

from __future__ import annotations

import json
import os
import shutil
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.dependencies.distribution import (
    read_dependency_distribution,
    write_dependency_distribution,
)
from fplinux_cli.dependencies.snapshots import preserve_inputs, publish_snapshot
from fplinux_cli.dependencies.transport import export_snapshot
from fplinux_cli.dependencies.workflow import checkout_declarations

from tests.cli_support import ROOT
from tests.host_process.dependencies.release_fixtures import make_release_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from pathlib import Path

    from tests.host_process.dependencies.release_fixtures import ReleaseCheckout

_ORIGINAL = "objects/sha256/ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"


@pytest.fixture
def release_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> ReleaseCheckout:
    """Give the fixture's real dependency selection a public CLI entry point."""
    monkeypatch.setenv("FPLINUX_DATA_DIR", ".data")
    checkout = make_release_checkout(tmp_path, monkeypatch)
    shutil.copy(ROOT / "fplinux", checkout.root / "fplinux")
    shutil.copytree(ROOT / "scripts/fplinux_cli", checkout.root / "scripts/fplinux_cli")
    return checkout


def _import_snapshot(
    checkout: ReleaseCheckout, parts: Path, output: Path
) -> subprocess.CompletedProcess[str]:
    return run_process(
        [str(checkout.root / "fplinux"), "dependencies", "import", str(parts), str(output)],
        name="dependency snapshot import",
        cwd=checkout.root,
        env=os.environ,
        timeout=10,
    )


def _parts_for_another_checkout(correct_parts: Path, wrong_parts: Path, cache: Path) -> None:
    """Use valid originals and checksums but save a different declaration context."""
    distribution = read_dependency_distribution(correct_parts, verify_parts=False)
    declarations, context = checkout_declarations()
    saved = wrong_parts.parent / "other-snapshot"
    original = wrong_parts.parent / "snapshot" / _ORIGINAL
    manifest = preserve_inputs(
        declarations,
        {**context, "selection": "other checkout"},
        saved,
        cache=cache,
        offline=True,
        sources=(original,),
    )
    publish_snapshot(saved, manifest)
    index = export_snapshot(
        saved, wrong_parts, prefix=f"FPLinux-{distribution.release.release_id}-inputs"
    )
    write_dependency_distribution(
        wrong_parts, release=distribution.release, snapshot=manifest["snapshot"], index=index
    )


@pytest.mark.parametrize(
    ("mismatch", "error"),
    [
        pytest.param(
            "snapshot",
            "dependency transport snapshot differs from its release metadata",
            id="snapshot",
        ),
        pytest.param(
            "checkout",
            "dependency snapshot does not match this checkout's declared external inputs",
            id="checkout",
        ),
    ],
)
def test_import_rejects_wrong_association_and_can_retry_at_the_same_destination(
    release_checkout: ReleaseCheckout, tmp_path: Path, mismatch: str, error: str
) -> None:
    good_parts, _snapshot = release_checkout.preserve(tmp_path / "right-parts")
    bad_parts = tmp_path / "wrong-parts"
    if mismatch == "snapshot":
        shutil.copytree(good_parts, bad_parts)
        metadata = bad_parts / "DEPENDENCIES.json"
        fields = json.loads(metadata.read_bytes())
        fields["snapshot"] = "0" * 64
        metadata.write_text(json.dumps(fields))
    else:
        _parts_for_another_checkout(good_parts, bad_parts, release_checkout.root / ".data")
    original_manifest = (tmp_path / "snapshot/manifest.json").read_bytes()
    output = tmp_path / "published/snapshot"
    output.parent.mkdir()
    sibling = output.parent / "previous"
    sibling.write_bytes(b"unrelated data")

    rejected = _import_snapshot(release_checkout, bad_parts, output)
    after_rejection = set(output.parent.iterdir())
    imported = _import_snapshot(release_checkout, good_parts, output)

    assert rejected.returncode == 1, rejected.stderr
    assert error in rejected.stderr
    assert imported.returncode == 0, imported.stderr
    assert after_rejection == {sibling}
    assert imported.stdout == f"Dependency snapshot: {output}\n"
    assert (output / "manifest.json").read_bytes() == original_manifest
    assert (output / _ORIGINAL).read_bytes() == b"abc"

    note = output / "note"
    note.write_bytes(b"existing snapshot metadata")
    repeated = _import_snapshot(release_checkout, good_parts, output)

    assert repeated.returncode == 1, repeated.stderr
    assert "dependency transport destination already exists" in repeated.stderr
    assert (output / "manifest.json").read_bytes() == original_manifest
    assert (output / _ORIGINAL).read_bytes() == b"abc"
    assert note.read_bytes() == b"existing snapshot metadata"
    assert sibling.read_bytes() == b"unrelated data"
