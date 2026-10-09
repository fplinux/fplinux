# SPDX-License-Identifier: GPL-2.0-only
"""Compile host C peers and own their disposable build directories."""

from __future__ import annotations

import shutil
from contextlib import contextmanager
from typing import TYPE_CHECKING

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator, Sequence
    from pathlib import Path

    import pytest


def compile_c(
    output: Path,
    arguments: Sequence[str],
    *,
    name: str,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    """Keep caller-owned compiler inputs and flags in their declared order."""
    return run_process(
        ["cc", *arguments, "-o", str(output)],
        name=name,
        timeout=30,
        check=check,
    )


@contextmanager
def c_build_directory(tmp_path_factory: pytest.TempPathFactory, name: str) -> Iterator[Path]:
    """Remove a fixture-owned build tree as soon as its consumer releases it."""
    directory = tmp_path_factory.mktemp(name)
    try:
        yield directory
    finally:
        shutil.rmtree(directory)
