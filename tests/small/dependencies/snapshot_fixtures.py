# SPDX-License-Identifier: GPL-2.0-only
"""Controlled checkout declarations for host snapshot restoration scenarios."""

from __future__ import annotations

from contextlib import contextmanager
from typing import TYPE_CHECKING, Any
from unittest import mock

from fplinux_cli import common
from fplinux_cli.dependencies import workflow as dependencies
from fplinux_cli.reporting import run

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

    from fplinux_cli.dependencies.inputs import DependencyInput


@contextmanager
def selected_checkout(
    root: Path, inputs: list[DependencyInput], context: dict[str, Any]
) -> Iterator[None]:
    """Replace declaration discovery while retaining real restore and reporting behavior."""
    with (
        mock.patch.object(common, "ROOT", root),
        mock.patch.object(run, "ROOT", root),
        mock.patch.object(dependencies, "checkout_declarations", return_value=(inputs, context)),
    ):
        yield
