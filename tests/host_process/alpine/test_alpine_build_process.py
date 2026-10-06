# SPDX-License-Identifier: GPL-2.0-only
"""Alpine packing process environment, working directory and stage-log routing."""

from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import tempfile
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.alpine import rootfs_files as alpine_builder
from fplinux_cli.reporting.run import RunReporter

from tests import ROOT
from tests.fixtures.executables import install_python_script


class AlpinePackingProcessTests:
    """A stub cpio child observes execution context; it does not create an archive."""

    @pytest.mark.parametrize(
        ("stage_enabled", "status"),
        [(False, 0), (False, 7), (True, 0), (True, 7)],
        ids=["plain-success", "plain-failure", "stage-success", "stage-failure"],
    )
    def test_packing_child_receives_build_environment_and_logs_errors(
        self, *, stage_enabled: bool, status: int
    ) -> None:
        """Stage routing preserves child context and rejects a nonzero packing result."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            root = temporary / "root"
            root.mkdir()
            (root / "payload").write_text("example\n")
            tools = temporary / "tools"
            tools.mkdir()
            cpio = tools / "cpio"
            install_python_script(ROOT / "tests/fixtures/processes/alpine_packing_cpio.py", cpio)
            destination = temporary / "rootfs.cpio"
            reporter = RunReporter("packing", temporary / "logs", "logs", verbose=False)
            stderr = io.StringIO()
            stdout = io.StringIO()
            stage = reporter.stage("pack") if stage_enabled else contextlib.nullcontext()
            with (
                mock.patch.dict(
                    os.environ,
                    {"PATH": f"{tools}:{os.environ['PATH']}", "FPLINUX_CPIO_STATUS": str(status)},
                ),
                contextlib.redirect_stderr(stderr),
                contextlib.redirect_stdout(stdout),
            ):
                if status:
                    if stage_enabled:
                        with pytest.raises(SystemExit) as stage_error, stage:
                            alpine_builder._write_rootfs_cpio(root, destination)  # noqa: SLF001
                        assert (stage_error.value.code) == (7)
                    else:
                        with pytest.raises(subprocess.CalledProcessError) as process_error:
                            alpine_builder._write_rootfs_cpio(root, destination)  # noqa: SLF001
                        assert (process_error.value.returncode) == (7)
                else:
                    with stage:
                        alpine_builder._write_rootfs_cpio(root, destination)  # noqa: SLF001
            reporter.finish()
            assert (json.loads(destination.read_text())) == (
                {"cwd": str(root), "locale": "C", "user": "fplinux"}
            )
            if stage_enabled:
                log = (temporary / "logs/01-pack.log").read_text()
                assert ("packing diagnostic") in (log)
                assert (stdout.getvalue()) == ("")
