# SPDX-License-Identifier: GPL-2.0-only
"""Host-process tests for the microSD growth decision boundary."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GROW = ROOT / "alpine/aports/fplinux-microsd-root/fplinux-microsd-grow"


class MicroSDGrowTests(unittest.TestCase):
    """Exercise the installed script with controlled external filesystem tools."""

    def setUp(self) -> None:
        """Create fake growpart and resize2fs process boundaries."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
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
        return subprocess.run(
            [str(GROW)],
            check=False,
            capture_output=True,
            text=True,
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

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self._calls(),
            [
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
            ],
        )

    def test_nochange_is_success_and_still_repairs_filesystem_size(self) -> None:
        """Growpart NOCHANGE still lets resize2fs repair an interrupted run."""
        result = self._run(1)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self._calls(),
            [
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
            ],
        )

    def test_partition_error_prevents_filesystem_growth(self) -> None:
        """A real partition failure is returned before resize2fs can run."""
        result = self._run(2)

        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stderr, "growpart failed\n")
        self.assertEqual(
            self._calls(),
            ["growpart:--update=on /dev/mmcblk0 2"],
        )

    def test_repeated_nochange_runs_have_no_persistent_marker(self) -> None:
        """Repeated complete runs succeed without creating persistent state."""
        first = self._run(1)
        second = self._run(1)

        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(
            self._calls(),
            [
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
            ],
        )

    def test_resize_failure_is_reported_after_partition_success(self) -> None:
        """A resize2fs failure is visible after successful partition handling."""
        result = self._run(0, resize2fs_status=3)

        self.assertEqual(result.returncode, 3)
        self.assertEqual(
            self._calls(),
            [
                "growpart:--update=on /dev/mmcblk0 2",
                "resize2fs:/dev/mmcblk0p2",
            ],
        )


if __name__ == "__main__":
    unittest.main()
