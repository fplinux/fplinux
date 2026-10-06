# SPDX-License-Identifier: GPL-2.0-only
"""Inspect the RAM boot archive produced from a captured staged source."""

from __future__ import annotations

import os
import subprocess
import sys
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli import common
from fplinux_cli.workspace import build_inputs as workspace_module
from fplinux_cli.workspace import staging as workspace_staging

if TYPE_CHECKING:
    from pathlib import Path


class StagedRamrootTests:
    """Keep archive bootstrap bytes bound to the captured target workspace."""

    def test_staged_ram_producer_reads_causal_bootstrap_snapshot(self, tmp_path: Path) -> None:
        """The normal staged producer reads boot source captured by the target snapshot."""
        snapshot = workspace_module.target_workspace_snapshot("nokia-ta1618")
        directory = tmp_path
        with mock.patch.object(workspace_staging, "ROOT", directory):
            source = workspace_staging.stage_workspace_snapshot(snapshot)
        bootstrap = source / "alpine/ramroot-init.sh"
        bootstrap.write_bytes(b"#!/bin/sh\nexit 0\n")
        with (
            mock.patch.object(workspace_module, "ROOT", source),
            mock.patch.object(workspace_staging, "ROOT", source),
            mock.patch.object(common, "ROOT", source),
        ):
            before = workspace_module.target_workspace_snapshot("nokia-ta1618")
            staged = workspace_staging.stage_workspace_snapshot(before)
            (source / "unrelated.txt").write_bytes(b"not a build input\n")
            unrelated = workspace_module.target_workspace_snapshot("nokia-ta1618")
            assert (before.recipe) == (unrelated.recipe)
            bootstrap.write_bytes(b"#!/bin/sh\nexit 1\n")
            changed = workspace_module.target_workspace_snapshot("nokia-ta1618")
            assert (before.recipe) != (changed.recipe)
        root = directory / "root"
        output = directory / "output"
        for relative in ("bin", "lib", "usr/lib"):
            (root / relative).mkdir(parents=True)
        output.mkdir()
        (root / "bin/busybox").write_bytes(b"runtime\n")
        (root / "lib/ld-musl-armhf.so.1").write_bytes(b"loader\n")
        (root / "lib/libc.musl-armv7.so.1").symlink_to("ld-musl-armhf.so.1")
        (root / "usr/lib/libgcc_s.so.1").write_bytes(b"gcc runtime\n")
        command = (
            "from pathlib import Path; import sys; "
            "from fplinux_cli.alpine.rootfs_files import _write_ramroot_initramfs; "
            "_write_ramroot_initramfs("
            "Path(sys.argv[1]), Path(sys.argv[2]))"
        )
        packed = subprocess.run(
            [
                sys.executable,
                "-B",
                "-c",
                command,
                str(root),
                str(output),
            ],
            cwd=staged,
            env={"PATH": os.environ["PATH"], "PYTHONPATH": str(staged / "scripts")},
            capture_output=True,
            text=True,
            check=False,
            timeout=20,
        )
        assert (packed.returncode) == (0), packed.stdout + packed.stderr
        boot = directory / "boot"
        boot.mkdir()
        with (output / "initramfs.cpio").open("rb") as archive:
            extracted = subprocess.run(
                ["cpio", "--quiet", "--extract", "--make-directories", "init"],
                stdin=archive,
                cwd=boot,
                capture_output=True,
                check=False,
                timeout=10,
            )
        assert (extracted.returncode) == (0), extracted.stderr
        assert ((boot / "init").read_bytes()) == (b"#!/bin/sh\nexit 0\n")
