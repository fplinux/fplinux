# SPDX-License-Identifier: GPL-2.0-only
"""Describe documentation registry inputs and their supported build consumer."""

from __future__ import annotations

import json
import re
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.manifests.source_catalog import resolve_source

from .inputs import DependencyInput, npm_declarations, source_input

if TYPE_CHECKING:
    from pathlib import Path


def _consumer(root: Path) -> dict[str, str]:
    """Select the same preserved Node and npm releases for local and Pages installs."""
    engines = json.loads((root / "package.json").read_text(encoding="utf-8")).get("engines")
    if not isinstance(engines, dict) or any(
        not isinstance(engines.get(name), str)
        or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", engines[name]) is None
        for name in ("node", "npm")
    ):
        fail("root package must declare exact Node.js and npm versions")
    if resolve_source(root, "site-node").version != engines["node"]:
        fail("documentation Node original must match the root package's exact version")
    if resolve_source(root, "npm").version != engines["npm"]:
        fail("documentation npm original must match the root package's exact version")
    return {
        "node": engines["node"],
        "npm": engines["npm"],
        "platform": "linux",
        "arch": "x86_64",
        "libc": "glibc",
    }


def site_dependency_selection(root: Path) -> tuple[list[DependencyInput], dict[str, object]]:
    """Select every locked registry archive without querying a package index."""
    context, inputs = npm_declarations(
        root / "site/package-lock.json",
        key_prefix="npm:site",
        cache_directory="downloads/site/npm",
        purpose="site-npm-package",
    )
    inputs.extend(source_input(resolve_source(root, source)) for source in ("site-node", "npm"))
    return inputs, {"consumer": _consumer(root), "npm": context}
