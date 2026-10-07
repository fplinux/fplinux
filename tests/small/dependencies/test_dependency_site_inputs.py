# SPDX-License-Identifier: GPL-2.0-only
"""Host tests for locked documentation registry inputs and snapshot restoration."""

from __future__ import annotations

import base64
import hashlib
import json
from typing import TYPE_CHECKING
from unittest.mock import patch

import pytest
from fplinux_cli.dependencies.site_inputs import site_dependency_selection
from fplinux_cli.dependencies.snapshots import preserve_inputs, publish_snapshot, restore_inputs

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def site_checkout(tmp_path: Path) -> Path:
    """Declare a Pages runtime and small locked packages without installing Node.js."""
    root = tmp_path / "checkout"
    (root / "site").mkdir(parents=True)
    workflow = root / ".github/workflows/pages.yml"
    workflow.parent.mkdir(parents=True)
    workflow.write_text(
        "jobs:\n"
        "  build:\n"
        "    runs-on: ubuntu-24.04\n"
        "    steps:\n"
        "      - with:\n"
        '          node-version: "24.21.0"\n'
        "      - run: npm install --global npm@12.2.0\n"
        "      - run: npm ci --prefix site\n"
        "  deploy:\n"
        "    runs-on: ubuntu-24.04\n"
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
    assert context["consumer"] == {
        "runner": "ubuntu-24.04",
        "node": "24.21.0",
        "npm": "12.2.0",
        "platform": "linux",
        "arch": "x86_64",
        "libc": "glibc",
        "libc_version": "2.39",
    }
    assert not (site_checkout / ".cache").exists()


def test_context_ignores_json_order_and_workflow_comments(site_checkout: Path) -> None:
    """Formatting leaves the declared external selection compatible with its snapshot."""
    before = site_dependency_selection(site_checkout)
    lock = site_checkout / "site/package-lock.json"
    lock.write_text(json.dumps(json.loads(lock.read_text()), sort_keys=True, indent=2))
    workflow = site_checkout / ".github/workflows/pages.yml"
    workflow.write_text(
        workflow.read_text().replace(
            'node-version: "24.21.0"', 'node-version: "24.21.0" # runtime'
        )
    )
    assert site_dependency_selection(site_checkout) == before


@pytest.mark.parametrize(
    ("old", "new", "message"),
    [
        ("ubuntu-24.04", "ubuntu-26.04", "supports Ubuntu 24.04"),
        ("24.21.0", "24.22.0", "root package's exact Node.js and npm versions"),
        ("npm@12.2.0", "npm@12.3.0", "root package's exact Node.js and npm versions"),
        ("npm@12.2.0", "npm@latest", "must install one exact npm version"),
    ],
    ids=["runner", "node", "npm", "unpinned-npm"],
)
def test_snapshot_selection_requires_the_pinned_pages_consumer(
    site_checkout: Path, old: str, new: str, message: str
) -> None:
    """A different runtime cannot silently reuse a snapshot selected for Pages."""
    workflow = site_checkout / ".github/workflows/pages.yml"
    workflow.write_text(workflow.read_text().replace(old, new))
    with pytest.raises(SystemExit, match=message):
        site_dependency_selection(site_checkout)


def test_snapshot_round_trip_restores_distinct_archives_with_the_same_filename(
    site_checkout: Path, tmp_path: Path
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
    restored = tmp_path / "restored-cache"
    with patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
        manifest = preserve_inputs(
            inputs,
            context,
            snapshot,
            cache=site_checkout / ".cache",
            offline=True,
            sources=[originals],
        )
        publish_snapshot(snapshot, manifest)
        restore_inputs(snapshot, inputs, context, cache=restored)
    assert len({item.destination for item in inputs}) == 2
    by_key = {item.key: restored / item.destination for item in inputs}
    assert by_key["npm:site:node_modules/first"].read_bytes() == b"first registry archive\n"
    assert by_key["npm:site:node_modules/second"].read_bytes() == b"second registry archive\n"
    assert all(path.is_relative_to(restored / "downloads/site/npm") for path in by_key.values())
    assert not (site_checkout / ".cache").exists()


def test_changed_registry_integrity_rejects_a_saved_snapshot(
    site_checkout: Path, tmp_path: Path
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
        sources=[original],
    )
    publish_snapshot(snapshot, manifest)
    package["integrity"] = "sha512-" + base64.b64encode(bytes.fromhex("33" * 64)).decode()
    lock.write_text(json.dumps({"packages": {"": {}, "node_modules/example": package}}))
    updated_inputs, updated_context = site_dependency_selection(site_checkout)
    restored = tmp_path / "restored-cache"
    with pytest.raises(SystemExit, match="does not match this checkout"):
        restore_inputs(snapshot, updated_inputs, updated_context, cache=restored)
    assert not restored.exists()
