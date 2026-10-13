# SPDX-License-Identifier: GPL-2.0-only
"""Host-process tests for the microSD growth decision boundary."""

from __future__ import annotations

import os
import shutil
import tempfile
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator

GROW = ROOT / "alpine/aports/fplinux-microsd-root/fplinux-microsd-grow"


@dataclass(frozen=True)
class GrowthContext:
    """Controlled filesystem-tool peers and their recorded calls for one case."""

    directory: Path
    calls: Path

    def run(
        self, growpart_status: int, resize2fs_status: int = 0
    ) -> subprocess.CompletedProcess[str]:
        """Run the growth script with controlled external-tool exit statuses."""
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

    def recorded_calls(self) -> list[str]:
        """Read the filesystem-tool actions observed by this case's executable peers."""
        if not self.calls.exists():
            return []
        return self.calls.read_text(encoding="utf-8").splitlines()


@pytest.fixture
def growth() -> Iterator[GrowthContext]:
    """Create fake growpart and resize2fs process boundaries."""
    with ExitStack() as cleanup:
        temporary = tempfile.TemporaryDirectory()
        cleanup.enter_context(temporary)
        directory = Path(temporary.name)
        for name, fixture in (("growpart", "growpart.sh"), ("resize2fs", "resize2fs.sh")):
            path = directory / name
            shutil.copyfile(ROOT / "tests/fixtures/rootfs" / fixture, path)
            path.chmod(0o755)
        yield GrowthContext(directory, directory / "calls")


class MicroSDGrowTests:
    """Exercise the installed script with controlled external filesystem tools."""

    def test_grown_partition_is_followed_by_filesystem_growth(self, growth: GrowthContext) -> None:
        """A successful partition change proceeds to filesystem growth."""
        result = growth.run(0)

        assert (result.returncode) == (0), result.stderr
        assert (growth.recorded_calls()) == (
            [
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
            ]
        )

    def test_nochange_is_success_and_still_repairs_filesystem_size(
        self, growth: GrowthContext
    ) -> None:
        """Growpart NOCHANGE still lets resize2fs repair an interrupted run."""
        result = growth.run(1)

        assert (result.returncode) == (0), result.stderr
        assert (growth.recorded_calls()) == (
            [
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
            ]
        )

    def test_partition_error_prevents_filesystem_growth(self, growth: GrowthContext) -> None:
        """A real partition failure is returned before resize2fs can run."""
        result = growth.run(2)

        assert (result.returncode) == (2)
        assert (result.stderr) == ("growpart failed\n")
        assert (growth.recorded_calls()) == (["growpart:--update=on /dev/mmcblk0 2"])

    def test_resize_failure_is_reported_after_partition_success(
        self, growth: GrowthContext
    ) -> None:
        """A resize2fs failure is visible after successful partition handling."""
        result = growth.run(0, resize2fs_status=3)

        assert (result.returncode) == (3)
        assert (growth.recorded_calls()) == (
            [
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
            ]
        )
