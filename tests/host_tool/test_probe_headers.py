# SPDX-License-Identifier: GPL-2.0-only
"""Clang preprocessing of project headers through the probe compiler command."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli import common, output
from fplinux_cli.cli import probe
from fplinux_cli.environment import kern
from fplinux_cli.output import RunReporter, Stage

from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Sequence

ROOT = Path(__file__).resolve().parents[2]


class ProbeHeaderTests(unittest.TestCase):
    """Use real Clang preprocessing, without Kern, a target sysroot or ARM linking."""

    def test_probe_accepts_both_project_header_include_forms(self) -> None:
        """Existing short includes and qualified includes resolve the project headers."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shutil.copytree(ROOT / "include", root / "include")
            source = root / "probe.c"
            destination = root / ".cache/tools/preprocessed.c"

            def preprocess_instead_of_kern(command: Sequence[str], **_kwargs: object) -> None:
                """Translate the container paths and run its compiler in preprocess-only mode."""
                compiler = list(command[command.index("--") + 1 :])
                compiler = [
                    argument.replace("/workspace/", f"{root}/").replace(
                        "/output/", f"{destination.parent}/"
                    )
                    for argument in compiler
                ]
                run_process(
                    [*compiler, "-E"],
                    name="preprocess project probe headers",
                    timeout=10,
                    check=True,
                )

            with (
                mock.patch.object(common, "ROOT", root),
                mock.patch.object(output, "ROOT", root),
                mock.patch.object(kern, "ROOT", root),
                mock.patch.object(Stage, "run", side_effect=preprocess_instead_of_kern),
            ):
                for header in ('"fplinux-keypad.h"', "<fplinux/fplinux-keypad.h>"):
                    with self.subTest(header=header):
                        source.write_text(
                            f"#include {header}\nint probe_header_check;\n",
                            encoding="utf-8",
                        )
                        reporter = RunReporter.create("probe-build", target=None, verbose=False)
                        probe._compile_probe(  # noqa: SLF001 -- real compiler command boundary.
                            source,
                            destination,
                            root / "unused-sysroot",
                            kern="stubbed-kern",
                            image="stubbed-image",
                            reporter=reporter,
                        )
                        self.assertIn("int probe_header_check;", destination.read_text())


if __name__ == "__main__":
    unittest.main()
