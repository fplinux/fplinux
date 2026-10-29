# SPDX-License-Identifier: GPL-2.0-only
"""Exercise public site commands with a controlled prepared tool boundary."""

from __future__ import annotations

import os
import shutil
from pathlib import Path

import pytest
from fplinux_cli.dependencies.site_install import site_runtime

from tests import ROOT
from tests.cli_support import prepare_cli_checkout
from tests.process import run_process


@pytest.mark.parametrize("exit_code", [0, 7])
def test_site_runs_pinned_tool_from_site_with_the_selected_data_root(
    tmp_path: Path, exit_code: int
) -> None:
    checkout = tmp_path / "checkout"
    checkout.mkdir()
    prepare_cli_checkout(checkout)
    shutil.copyfile(ROOT / "sources.lock.toml", checkout / "sources.lock.toml")
    (checkout / "site/node_modules").mkdir(parents=True)
    cache = tmp_path / "external-data"
    runtime = site_runtime(checkout, cache)
    (runtime / "bin").mkdir(parents=True)
    peer = ROOT / "tests/public_workflow/fixtures/site/npm-peer.sh"
    for executable in (runtime / "bin/npm", runtime / "bin/node"):
        shutil.copyfile(peer, executable)
        executable.chmod(0o755)
    (cache / "site/node").symlink_to(Path("runtime") / runtime.name)
    trace = tmp_path / "trace"

    result = run_process(
        [str(checkout / "fplinux"), "site", "build-offline"],
        name="site command",
        cwd=tmp_path,
        env=os.environ
        | {
            "FPLINUX_DATA_DIR": str(cache),
            "FPLINUX_SITE_TEST_TRACE": str(trace),
            "FPLINUX_SITE_TEST_EXIT": str(exit_code),
        },
        timeout=20,
    )

    assert result.returncode == exit_code, result.stderr
    assert trace.read_text().splitlines() == [
        str(checkout / "site"),
        "run build:offline",
        "true",
        str(cache),
    ]


def test_site_missing_tools_suggests_setup_without_calling_ambient_npm(tmp_path: Path) -> None:
    checkout = tmp_path / "checkout"
    checkout.mkdir()
    prepare_cli_checkout(checkout)
    shutil.copyfile(ROOT / "sources.lock.toml", checkout / "sources.lock.toml")
    cache = tmp_path / "external-data"
    ambient = tmp_path / "ambient"
    ambient.mkdir()
    peer = ambient / "npm"
    shutil.copyfile(ROOT / "tests/public_workflow/fixtures/site/npm-peer.sh", peer)
    peer.chmod(0o755)
    trace = tmp_path / "trace"

    result = run_process(
        [str(checkout / "fplinux"), "site", "check"],
        name="site without prepared tools",
        cwd=tmp_path,
        env=os.environ
        | {
            "FPLINUX_DATA_DIR": str(cache),
            "FPLINUX_SITE_TEST_TRACE": str(trace),
            "PATH": str(ambient) + os.pathsep + os.environ["PATH"],
        },
        timeout=20,
    )

    assert result.returncode == 1
    assert "run ./fplinux setup --site" in result.stderr
    assert not trace.exists()
