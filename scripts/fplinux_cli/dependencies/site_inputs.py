# SPDX-License-Identifier: GPL-2.0-only
"""Describe documentation registry inputs and their supported build consumer."""

from __future__ import annotations

import json
import re
from typing import TYPE_CHECKING, Any, TypedDict

from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.source_catalog import resolve_source_record

from .inputs import DependencyInput, npm_declarations, source_input

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.manifests.source_catalog import ResolvedSource


class SiteConsumer(TypedDict):
    """Runtime identity and platform for the site lock."""

    node: str
    npm: str
    platform: str
    arch: str
    libc: str


def site_runtime_sources(root: Path) -> tuple[ResolvedSource, ResolvedSource]:
    """Resolve both runtime originals from the same source catalog."""
    catalog = load_toml(root / "sources.lock.toml")
    return (
        resolve_source_record("site-node", catalog.get("site-node")),
        resolve_source_record("npm", catalog.get("npm")),
    )


def _consumer(root: Path, node: ResolvedSource, npm: ResolvedSource) -> SiteConsumer:
    """Select the same preserved Node and npm releases for local and Pages installs."""
    engines = json.loads((root / "package.json").read_text(encoding="utf-8")).get("engines")
    if not isinstance(engines, dict) or any(
        not isinstance(engines.get(name), str)
        or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", engines[name]) is None
        for name in ("node", "npm")
    ):
        fail("root package must declare exact Node.js and npm versions")
    if node.version != engines["node"]:
        fail("documentation Node original must match the root package's exact version")
    if npm.version != engines["npm"]:
        fail("documentation npm original must match the root package's exact version")
    return {
        "node": engines["node"],
        "npm": engines["npm"],
        "platform": "linux",
        "arch": "x86_64",
        "libc": "glibc",
    }


def site_dependency_selection(root: Path) -> tuple[list[DependencyInput], dict[str, Any]]:
    """Select every locked registry archive without querying a package index."""
    context, inputs = npm_declarations(
        root / "site/package-lock.json",
        key_prefix="npm:site",
        cache_directory="downloads/site/npm",
        purpose="site-npm-package",
    )
    node, npm = site_runtime_sources(root)
    inputs.extend((source_input(node), source_input(npm)))
    return inputs, {"consumer": _consumer(root, node, npm), "npm": context}
