# SPDX-License-Identifier: GPL-2.0-only
"""Validate the repository build-image source policy."""

from __future__ import annotations

import os
from pathlib import Path

from fplinux_cli import common
from fplinux_cli.common import fail


def validate_source_policy() -> None:
    """Enforce the single-image source policy."""
    ignored = {".cache", ".git"}
    containerfiles: list[Path] = []
    # Prune the generated trees before descending: they hold millions of
    # cached build paths that a plain rglob would stat one by one.
    for directory, subdirectories, names in os.walk(common.ROOT):
        subdirectories[:] = [name for name in subdirectories if name not in ignored]
        for name in names:
            if name != "Containerfile":
                continue
            relative = (Path(directory) / name).relative_to(common.ROOT)
            containerfiles.append(relative)
    containerfiles.sort()
    if containerfiles != [Path("Containerfile")]:
        fail("source must contain exactly one root Containerfile")
    instructions = [
        line.strip()
        for line in (common.ROOT / containerfiles[0]).read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    from_instructions = [line for line in instructions if line.upper().startswith("FROM ")]
    if from_instructions != ["FROM ${BASE_IMAGE}"] or instructions[0] != "ARG BASE_IMAGE":
        fail("Containerfile must use one lock-provided FROM ${BASE_IMAGE}")
