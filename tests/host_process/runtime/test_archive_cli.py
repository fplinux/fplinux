# SPDX-License-Identifier: GPL-2.0-only
"""Run the delivered shell wrapper with the actual standalone Python closure."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.fixtures.portable_archive import (
    portable_archive as portable_archive,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.fixtures.portable_archive import PortableArchive


@pytest.mark.parametrize(
    ("arguments", "command_help", "status", "message"),
    [
        (["--help"], "run", 0, "--from-dump"),
        (["run", "--help"], "run", 0, "--from-dump"),
        (["console", "--help"], "console", 0, "--allow-other-build"),
        (["verify", "--help"], "verify", 0, "--phone-data"),
        (["prepare", "--help"], "prepare", 0, "--from-dump"),
        (["nand", "identify", "--help"], "nand identify", 0, "--bundle"),
        (["run", "--reconnect"], None, 2, "unrecognized arguments: --reconnect"),
        (["verify", "--exec", "true"], None, 2, "unrecognized arguments: --exec true"),
        (
            ["verify"],
            None,
            1,
            "No known live phone connection is available",
        ),
        (
            ["console", "--allow-other-build", "--exec", "printf ready"],
            None,
            1,
            "No known live phone connection is available",
        ),
    ],
    ids=[
        "default-run",
        "explicit-run",
        "explicit-console",
        "explicit-verify",
        "explicit-prepare",
        "explicit-nand",
        "removed-reconnect",
        "verify-rejects-remote-action",
        "verify-without-session",
        "delivered-diagnostic-console",
    ],
)
def test_shell_launcher_routes_commands_through_delivered_runtime(
    portable_archive: PortableArchive,
    arguments: list[str],
    command_help: str | None,
    status: int,
    message: str,
) -> None:
    """Use the requested command and stop without a boot on a missing session."""
    archive = portable_archive
    loader = archive.root / "runtime/lib/ld-musl-x86_64.so.1"
    loader.parent.mkdir(parents=True)
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/archive-cli/archive_loader.py", loader
    )

    result = subprocess.run(
        ["sh", str(archive.root / "Run.sh"), *arguments],
        cwd=archive.root.parent,
        env=archive.environment(),
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )

    assert result.returncode == status, result.stdout + result.stderr
    if command_help is not None:
        assert result.stdout.startswith(f"usage: fplinux.py {command_help} ")
    assert message in result.stdout + result.stderr
    assert not list(archive.root.rglob("__pycache__"))
