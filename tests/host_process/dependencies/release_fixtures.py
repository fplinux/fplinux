# SPDX-License-Identifier: GPL-2.0-only
"""Own a complete synthetic checkout for local release-input scenarios."""

from __future__ import annotations

import io
import re
import shutil
import subprocess
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import tomlkit
from fplinux_cli import common
from fplinux_cli.alpine.registration import COMMON_PACKAGES
from fplinux_cli.alpine.selection import aport_build_order
from fplinux_cli.artifacts.release import capture_public_sources
from fplinux_cli.dependencies.distribution import read_dependency_distribution
from fplinux_cli.dependencies.workflow import (
    checkout_declarations,
    create_dependencies,
    export_dependencies,
)
from fplinux_cli.reporting import run as reporting

if TYPE_CHECKING:
    import pytest

_ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
_ABC_SHA512 = (
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"
)


@dataclass(frozen=True)
class ReleaseCheckout:
    """Expose the owned source root and its independently resolved Git commit."""

    root: Path
    commit: str

    @property
    def release_id(self) -> str:
        """Git's full commit defines the public short release ID for this clean tree."""
        return self.commit[:12]

    def preserve(self, output: Path) -> tuple[Path, str]:
        """Create real inputs-only metadata, compression and content checksums."""
        declarations, _context = checkout_declarations()
        for declaration in declarations:
            original = self.root / ".data" / declaration.destination
            original.parent.mkdir(parents=True, exist_ok=True)
            original.write_bytes(b"abc")
        snapshot = output.parent / "snapshot"
        create_dependencies(snapshot, offline=True, sources=(), inputs_only=True)
        export_dependencies(snapshot, output)
        shutil.rmtree(self.root / ".data/downloads")
        return output, read_dependency_distribution(output).snapshot

    def assert_originals_restored(self) -> None:
        """Check the native source and APK consumer slots against literal originals."""
        assert (
            self.root / ".data/downloads/sources/support/support.tar.gz"
        ).read_bytes() == b"abc"
        assert (
            self.root / ".data/downloads/alpine/packages/runtime-1-r0.apk"
        ).read_bytes() == b"abc"
        assert (
            self.root / ".data/downloads/environment/apk/main/x86_64/tool-2.0-r0.apk"
        ).read_bytes() == b"abc"
        assert not (self.root / ".data/environment").exists()


def make_release_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> ReleaseCheckout:
    """Use real selection and source capture, while all declared originals contain abc."""
    root = tmp_path / "checkout"
    shutil.copytree(Path(__file__).parents[2] / "fixtures/dependencies/checkout", root)
    (root / "sources.lock.toml.fixture").rename(root / "sources.lock.toml")
    for path in root.rglob("*.toml"):
        text = re.sub(r"[a-f0-9]{64}", _ABC_SHA256, path.read_text())
        text = re.sub(r"bytes = [0-9]+", "bytes = 3", text)
        path.write_text(text)
    sources = root / "sources.lock.toml"
    document = tomlkit.parse(sources.read_text())
    support = document["support"]
    assert isinstance(support, tomlkit.items.Table)
    support["algorithm"] = "sha512"
    support["checksum"] = _ABC_SHA512
    sources.write_text(
        tomlkit.dumps(document)
        + (Path(__file__).parent / "fixtures/release-inputs/site-sources.toml").read_text()
    )
    for name in ("package.json", "package-lock.json", "site/package-lock.json"):
        path = root / name
        path.parent.mkdir(exist_ok=True)
        shutil.copyfile(
            Path(__file__).parent / "fixtures/release-inputs" / name.replace("/", "-"), path
        )
    target = root / "targets/test-phone/target.toml"
    target.write_text(target.read_text().replace('["fplinux-bash", "fplinux-libtsm"]', "[]"))
    for package in aport_build_order((*COMMON_PACKAGES, "fplinux-debug-tools")):
        recipe = root / "alpine/aports" / package / "APKBUILD"
        recipe.parent.mkdir(parents=True)
        recipe.write_text(f'pkgname={package}\npkgver=1\npkgrel=0\narch="armv7"\n')
    for arguments in (
        ("init", "--quiet"),
        ("config", "user.name", "Test Author"),
        ("config", "user.email", "test@example.invalid"),
        ("add", "."),
        ("commit", "--quiet", "--no-gpg-sign", "-m", "Release inputs fixture"),
    ):
        subprocess.run(["git", "-C", str(root), *arguments], capture_output=True, check=True)
    commit = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        capture_output=True,
        check=True,
        text=True,
    ).stdout.strip()
    monkeypatch.setattr(common, "ROOT", root)
    monkeypatch.setattr(reporting, "ROOT", root)

    def reject_network(request: urllib.request.Request, *, timeout: int) -> io.BytesIO:
        raise AssertionError(f"unexpected HTTP request: {request.full_url}; timeout={timeout}")

    monkeypatch.setattr(urllib.request, "urlopen", reject_network)
    assert capture_public_sources().release.release_id == commit[:12]
    return ReleaseCheckout(root, commit)


def serve_release_bytes(
    monkeypatch: pytest.MonkeyPatch, directory: Path, release_id: str
) -> list[str]:
    """Replace only HTTP response bytes; downloads retain hashing and atomic publication."""
    requested: list[str] = []
    base = f"https://github.com/fplinux/fplinux/releases/download/build-{release_id}/"

    def open_url(request: urllib.request.Request, *, timeout: int) -> io.BytesIO:
        assert timeout == 60
        assert request.full_url.startswith(base)
        assert not request.has_header("Authorization")
        name = request.full_url.removeprefix(base)
        requested.append(name)
        return io.BytesIO((directory / name).read_bytes())

    monkeypatch.setattr(urllib.request, "urlopen", open_url)
    return requested
