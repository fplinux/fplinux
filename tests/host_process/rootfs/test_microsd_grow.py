# SPDX-License-Identifier: GPL-2.0-only
"""Host-process tests for the microSD growth decision boundary."""

from __future__ import annotations

import os
import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator

GROW = ROOT / "alpine/aports/fplinux-microsd-root/fplinux-microsd-grow"


class MicroSDGrowTests:
    """Exercise the installed script with controlled external filesystem tools."""

    @pytest.fixture(autouse=True)
    def _prepare_case(self) -> Iterator[None]:
        """Create fake growpart and resize2fs process boundaries."""
        with ExitStack() as cleanup:
            self.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(self.temporary)
            self.directory = Path(self.temporary.name)
            self.calls = self.directory / "calls"
            self._tool(
                "growpart",
                """#!/bin/sh
printf 'growpart:%s\\n' "$*" >> "$FPLINUX_GROW_CALLS"
if [ "${FPLINUX_GROWPART_STATUS:?}" -ge 2 ]; then
    printf 'growpart failed\\n' >&2
fi
exit "$FPLINUX_GROWPART_STATUS"
""",
            )
            self._tool(
                "resize2fs",
                """#!/bin/sh
printf 'resize2fs:%s\\n' "$*" >> "$FPLINUX_GROW_CALLS"
exit "${FPLINUX_RESIZE2FS_STATUS:-0}"
""",
            )
            yield

    def _tool(self, name: str, source: str) -> None:
        path = self.directory / name
        path.write_text(source, encoding="utf-8")
        path.chmod(0o755)

    def _run(
        self, growpart_status: int, resize2fs_status: int = 0
    ) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment.update(
            {
                "PATH": f"{self.directory}:/usr/bin:/bin",
                "FPLINUX_GROW_CALLS": str(self.calls),
                "FPLINUX_GROWPART_STATUS": str(growpart_status),
                "FPLINUX_RESIZE2FS_STATUS": str(resize2fs_status),
            }
        )
        return run_process(
            [str(GROW)],
            name="microSD growth policy",
            timeout=5,
            env=environment,
        )

    def _calls(self) -> list[str]:
        if not self.calls.exists():
            return []
        return self.calls.read_text(encoding="utf-8").splitlines()

    def test_grown_partition_is_followed_by_filesystem_growth(self) -> None:
        """A successful partition change proceeds to filesystem growth."""
        result = self._run(0)

        assert (result.returncode) == (0), result.stderr
        assert (self._calls()) == (
            [
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
            ]
        )

    def test_nochange_is_success_and_still_repairs_filesystem_size(self) -> None:
        """Growpart NOCHANGE still lets resize2fs repair an interrupted run."""
        result = self._run(1)

        assert (result.returncode) == (0), result.stderr
        assert (self._calls()) == (
            [
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
            ]
        )

    def test_partition_error_prevents_filesystem_growth(self) -> None:
        """A real partition failure is returned before resize2fs can run."""
        result = self._run(2)

        assert (result.returncode) == (2)
        assert (result.stderr) == ("growpart failed\n")
        assert (self._calls()) == (["growpart:--update=on /dev/mmcblk0 2"])

    def test_resize_failure_is_reported_after_partition_success(self) -> None:
        """A resize2fs failure is visible after successful partition handling."""
        result = self._run(0, resize2fs_status=3)

        assert (result.returncode) == (3)
        assert (self._calls()) == (
            [
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
            ]
        )
