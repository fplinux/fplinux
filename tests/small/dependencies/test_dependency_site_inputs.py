# SPDX-License-Identifier: GPL-2.0-only
"""Host tests for locked documentation registry inputs and snapshot restoration."""

from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path
from unittest.mock import patch

import pytest
from fplinux_cli.dependencies.site_inputs import site_dependency_selection
from fplinux_cli.dependencies.snapshots import preserve_inputs, publish_snapshot
from fplinux_cli.dependencies.workflow import restore_dependencies

from tests.small.dependencies.snapshot_fixtures import selected_checkout

_FIXTURES = Path(__file__).parent / "fixtures/site-inputs"


@pytest.fixture
def site_checkout(tmp_path: Path) -> Path:
    """Declare exact runtime originals and locked packages without installing Node.js."""
    root = tmp_path / "checkout"
    (root / "site").mkdir(parents=True)
    (root / "sources.lock.toml").write_bytes(
        (_FIXTURES / "sources.lock.toml.fixture").read_bytes()
    )
    (root / "package.json").write_text('{"engines":{"node":"24.21.0","npm":"12.2.0"}}\n')
    (root / "site/package-lock.json").write_text(
        json.dumps(
            {
                "packages": {
                    "": {"name": "documentation", "dependencies": {"example": "1.0.0"}},
                    "node_modules/example": {
                        "version": "1.0.0",
                        "resolved": "https://registry.example.invalid/example-1.0.0.tgz",
                        "integrity": "sha512-"
                        + base64.b64encode(bytes.fromhex("11" * 64)).decode(),
                    },
                    "node_modules/@example/windows": {
                        "version": "1.0.0",
                        "resolved": "https://registry.example.invalid/windows-1.0.0.tgz",
                        "integrity": "sha256-"
                        + base64.b64encode(bytes.fromhex("22" * 32)).decode(),
                        "optional": True,
                        "os": ["win32"],
                        "cpu": ["x64"],
                    },
                }
            }
        )
    )
    return root


@pytest.fixture
def runtime_original(tmp_path: Path) -> Path:
    """Supply the small verified bytes declared by both synthetic runtime originals."""
    original = tmp_path / "runtime-original"
    original.write_bytes(b"abc")
    return original


def test_locked_registry_inputs_keep_integrity_and_optional_platform_archives(
    site_checkout: Path,
) -> None:
    """Snapshot selection preserves exact originals even for an optional other-platform package."""
    with patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
        inputs, context = site_dependency_selection(site_checkout)
    actual = {item.key: item for item in inputs}
    example = actual["npm:site:node_modules/example"]
    assert example.url == "https://registry.example.invalid/example-1.0.0.tgz"
    assert example.checksum == "11" * 64
    assert example.algorithm == "sha512"
    assert example.sha256 is None
    assert example.purpose == "site-npm-package"
    optional = actual["npm:site:node_modules/@example/windows"]
    assert optional.url == "https://registry.example.invalid/windows-1.0.0.tgz"
    assert optional.sha256 == "22" * 32
    assert optional.algorithm == "sha256"
    assert actual["source:site-node"].url == "https://example.invalid/node.tar.xz"
    assert actual["source:site-node"].destination == "downloads/sources/site-node/node.tar.xz"
    assert actual["source:npm"].url == "https://example.invalid/npm.tgz"
    assert actual["source:npm"].destination == "downloads/sources/npm/npm.tgz"
    assert context["consumer"] == {
        "node": "24.21.0",
        "npm": "12.2.0",
        "platform": "linux",
        "arch": "x86_64",
        "libc": "glibc",
    }
    assert not (site_checkout / ".cache").exists()


def test_context_ignores_lock_mapping_order(site_checkout: Path) -> None:
    """Formatting leaves the declared external selection compatible with its snapshot."""
    before = site_dependency_selection(site_checkout)
    lock = site_checkout / "site/package-lock.json"
    lock.write_text(json.dumps(json.loads(lock.read_text()), sort_keys=True, indent=2))
    assert site_dependency_selection(site_checkout) == before


@pytest.mark.parametrize(
    ("engine", "version", "message"),
    [
        ("node", "24.22.0", "Node original must match the root package's exact version"),
        ("npm", "12.3.0", "npm original must match the root package's exact version"),
        ("node", ">=24", "root package must declare exact Node.js and npm versions"),
        ("npm", "latest", "root package must declare exact Node.js and npm versions"),
    ],
    ids=["different-node", "different-npm", "unpinned-node", "unpinned-npm"],
)
def test_snapshot_selection_requires_engines_to_match_runtime_originals(
    site_checkout: Path, engine: str, version: str, message: str
) -> None:
    """Declared engine versions cannot diverge from the preserved executable releases."""
    package = site_checkout / "package.json"
    declaration = json.loads(package.read_text())
    declaration["engines"][engine] = version
    package.write_text(json.dumps(declaration))
    with pytest.raises(SystemExit, match=message):
        site_dependency_selection(site_checkout)


def test_snapshot_round_trip_restores_distinct_archives_with_the_same_filename(
    site_checkout: Path, runtime_original: Path, tmp_path: Path
) -> None:
    """Native integrity checks and separated cache destinations preserve both original archives."""
    first = b"first registry archive\n"
    second = b"second registry archive\n"
    lock = site_checkout / "site/package-lock.json"
    lock.write_text(
        json.dumps(
            {
                "packages": {
                    "": {"name": "documentation"},
                    "node_modules/first": {
                        "version": "1.0.0",
                        "resolved": "https://registry.example.invalid/first/package.tgz",
                        "integrity": "sha512-"
                        + base64.b64encode(hashlib.sha512(first).digest()).decode(),
                    },
                    "node_modules/second": {
                        "version": "1.0.0",
                        "resolved": "https://registry.example.invalid/second/package.tgz",
                        "integrity": "sha256-"
                        + base64.b64encode(hashlib.sha256(second).digest()).decode(),
                    },
                }
            }
        )
    )
    inputs, context = site_dependency_selection(site_checkout)
    originals = tmp_path / "originals"
    originals.mkdir()
    (originals / "one.tgz").write_bytes(first)
    (originals / "two.tgz").write_bytes(second)
    snapshot = tmp_path / "snapshot"
    restored_root = tmp_path / "restored-checkout"
    restored = restored_root / ".cache"
    with patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
        manifest = preserve_inputs(
            inputs,
            context,
            snapshot,
            cache=site_checkout / ".cache",
            offline=True,
            sources=[originals, runtime_original],
        )
        publish_snapshot(snapshot, manifest)
        with selected_checkout(restored_root, inputs, context):
            restore_dependencies(snapshot, inputs_only=True)
    by_key = {item.key: restored / item.destination for item in inputs}
    assert by_key["npm:site:node_modules/first"].read_bytes() == b"first registry archive\n"
    assert by_key["npm:site:node_modules/second"].read_bytes() == b"second registry archive\n"
    assert by_key["npm:site:node_modules/first"] != by_key["npm:site:node_modules/second"]
    assert all(
        path.is_relative_to(restored / "downloads/site/npm")
        for key, path in by_key.items()
        if key.startswith("npm:site:")
    )
    assert (restored / "downloads/sources/site-node/node.tar.xz").read_bytes() == b"abc"
    assert (restored / "downloads/sources/npm/npm.tgz").read_bytes() == b"abc"
    assert not (site_checkout / ".cache").exists()


def test_changed_registry_integrity_rejects_a_saved_snapshot(
    site_checkout: Path, runtime_original: Path, tmp_path: Path
) -> None:
    """A package declaration update cannot restore a previous original as the new input."""
    contents = b"original registry archive\n"
    lock = site_checkout / "site/package-lock.json"
    package = {
        "version": "1.0.0",
        "resolved": "https://registry.example.invalid/example-1.0.0.tgz",
        "integrity": "sha512-" + base64.b64encode(hashlib.sha512(contents).digest()).decode(),
    }
    lock.write_text(json.dumps({"packages": {"": {}, "node_modules/example": package}}))
    inputs, context = site_dependency_selection(site_checkout)
    original = tmp_path / "original.tgz"
    original.write_bytes(contents)
    snapshot = tmp_path / "snapshot"
    manifest = preserve_inputs(
        inputs,
        context,
        snapshot,
        cache=site_checkout / ".cache",
        offline=True,
        sources=[original, runtime_original],
    )
    publish_snapshot(snapshot, manifest)
    package["integrity"] = "sha512-" + base64.b64encode(bytes.fromhex("33" * 64)).decode()
    lock.write_text(json.dumps({"packages": {"": {}, "node_modules/example": package}}))
    updated_inputs, updated_context = site_dependency_selection(site_checkout)
    restored_root = tmp_path / "restored-checkout"
    with (
        selected_checkout(restored_root, updated_inputs, updated_context),
        pytest.raises(SystemExit, match="does not match this checkout"),
    ):
        restore_dependencies(snapshot, inputs_only=True)
    assert not (restored_root / ".cache/downloads").exists()
