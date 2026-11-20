# SPDX-License-Identifier: GPL-2.0-only
"""Collect active source bytes from a controlled checkout without private data."""

from __future__ import annotations

import io
import tarfile

import pytest
from fplinux_cli.manifests.loaders import LoaderSource
from fplinux_cli.packaging.sources import _loader_license_notices


def _source_archive(members: dict[str, bytes]) -> bytes:
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode="w:gz") as archive:
        for name, contents in members.items():
            member = tarfile.TarInfo(name)
            member.size = len(contents)
            archive.addfile(member, io.BytesIO(contents))
    return payload.getvalue()


def test_loader_distribution_retains_exact_upstream_license_files() -> None:
    """Runtime notices preserve original bytes while full sources stay in the companion."""
    sources = tuple(
        LoaderSource(
            role, role, "https://example.invalid/source", "a" * 64, f"loaders/{role}.tar.gz", role
        )
        for role in ("fdl1", "fdl2", "sml")
    )
    files = {
        "upstream/loaders/fdl1.tar.gz": _source_archive({"fdl1/main.c": b"original copyright\n"}),
        "upstream/loaders/fdl2.tar.gz": _source_archive(
            {
                "fdl2/Licenses/README": b"original license explanation\n",
                "fdl2/Licenses/gpl-2.0.txt": b"original GPL terms\n",
                "fdl2/main.c": b"original board code\n",
            }
        ),
        "upstream/loaders/sml.tar.gz": _source_archive(
            {
                "sml/license.rst": b"See docs/license.rst\n",
                "sml/docs/license.rst": b"original monitor license\n",
                "sml/main.c": b"monitor code\n",
            }
        ),
    }
    notices = _loader_license_notices(sources, files)
    assert notices == {
        "licenses/loaders/fdl2/README": b"original license explanation\n",
        "licenses/loaders/fdl2/gpl-2.0.txt": b"original GPL terms\n",
        "licenses/loaders/sml/docs/license.rst": b"original monitor license\n",
    }


@pytest.mark.parametrize("role", ["fdl2", "sml"])
def test_loader_distribution_rejects_an_archive_missing_its_license_files(role: str) -> None:
    source = LoaderSource(
        role, role, "https://example.invalid/source", "a" * 64, f"loaders/{role}.tar.gz", role
    )
    files = {f"upstream/loaders/{role}.tar.gz": _source_archive({f"{role}/main.c": b"source"})}
    with pytest.raises(SystemExit, match=r"license.*missing"):
        _loader_license_notices((source,), files)
