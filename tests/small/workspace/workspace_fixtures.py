# SPDX-License-Identifier: GPL-2.0-only
"""Owned source files shared by immutable workspace scenarios."""

from __future__ import annotations

import unittest
from contextlib import contextmanager
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli.workspace import build_inputs, quality_inputs, staging

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


class WorkspaceSourceFixture(unittest.TestCase):
    """Create an owned source file with explicit contents and mode."""

    def _source(self, root: Path, *, contents: bytes = b"source", mode: int = 0o754) -> Path:
        source = root / "source"
        source.write_bytes(contents)
        source.chmod(mode)
        return source

    def _registration_source(self, root: Path) -> tuple[str, Path]:
        """Create the package declarations materialized beside a synthetic target."""
        relative = "scripts/fplinux_cli/alpine/registration.py"
        source = root / relative
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(b"# empty package declarations\n")
        return relative, source


@contextmanager
def workspace_root(root: Path) -> Iterator[None]:
    """Bind snapshot selection and materialization to one owned temporary checkout."""
    with (
        mock.patch.object(build_inputs, "ROOT", root),
        mock.patch.object(quality_inputs, "ROOT", root),
        mock.patch.object(staging, "ROOT", root),
    ):
        yield


@contextmanager
def empty_package_graph() -> Iterator[None]:
    """Keep package selection empty when a snapshot fixture supplies only source inputs."""
    with (
        mock.patch.object(build_inputs, "selected_packages", return_value=()),
        mock.patch.object(build_inputs, "bundle_packages", return_value=()),
        mock.patch.object(build_inputs, "selected_aport_graph", return_value=()),
    ):
        yield
