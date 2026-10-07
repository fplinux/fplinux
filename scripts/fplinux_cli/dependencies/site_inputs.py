# SPDX-License-Identifier: GPL-2.0-only
"""Describe documentation registry inputs and their supported build consumer."""

from __future__ import annotations

import json
import re
import shlex
from typing import TYPE_CHECKING

from fplinux_cli.common import fail

from .inputs import DependencyInput, npm_declarations

if TYPE_CHECKING:
    from pathlib import Path


def _consumer(root: Path) -> dict[str, str]:
    """Bind the documentation snapshot to the pinned Pages runtime."""
    workflow = (root / ".github/workflows/pages.yml").read_text(encoding="utf-8")
    build = re.search(r"^  build:\s*(?:#.*)?$", workflow, re.MULTILINE)
    if build is None:
        fail("documentation workflow has no supported build job")
    remaining = workflow[build.end() :]
    next_job = re.search(r"^  \S", remaining, re.MULTILINE)
    block = remaining[: next_job.start()] if next_job is not None else remaining
    values: dict[str, str] = {}
    for field in ("runs-on", "node-version"):
        matches = re.findall(rf"^\s+{field}:\s*(.+)$", block, re.MULTILINE)
        if len(matches) != 1:
            fail(f"documentation workflow needs one {field} value in its build job")
        words = shlex.split(matches[0], comments=True)
        if len(words) != 1:
            fail(f"documentation workflow has an unsupported {field} value")
        values[field] = words[0]
    npm_versions = re.findall(r"\bnpm install (?:--global|-g) npm@([0-9.]+)\b", block)
    if len(npm_versions) != 1:
        fail("documentation workflow must install one exact npm version")
    engines = json.loads((root / "package.json").read_text(encoding="utf-8")).get("engines")
    if not isinstance(engines, dict) or any(
        not isinstance(engines.get(name), str)
        or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", engines[name]) is None
        for name in ("node", "npm")
    ):
        fail("root package must declare exact Node.js and npm versions")
    if values["node-version"] != engines["node"] or npm_versions[0] != engines["npm"]:
        fail("documentation workflow must use the root package's exact Node.js and npm versions")
    if values["runs-on"] != "ubuntu-24.04":
        fail("documentation dependency preservation supports Ubuntu 24.04")
    return {
        "runner": values["runs-on"],
        "node": engines["node"],
        "npm": engines["npm"],
        "platform": "linux",
        "arch": "x86_64",
        "libc": "glibc",
        "libc_version": "2.39",
    }


def site_dependency_selection(root: Path) -> tuple[list[DependencyInput], dict[str, object]]:
    """Select every locked registry archive without querying a package index."""
    context, inputs = npm_declarations(
        root / "site/package-lock.json",
        key_prefix="npm:site",
        cache_directory="downloads/site/npm",
        purpose="site-npm-package",
    )
    return inputs, {"consumer": _consumer(root), "npm": context}
