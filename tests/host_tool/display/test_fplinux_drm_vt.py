# SPDX-License-Identifier: GPL-2.0-only
"""Exercise guardian process cleanup with a substituted VT ioctl boundary."""

from __future__ import annotations

import shlex
import tempfile
import unittest
from pathlib import Path

from tests import ROOT
from tests.process import run_process


class DrmVtGuardianTests(unittest.TestCase):
    """Check process lifetime and return requests without claiming a live VT."""

    executable: Path
    guardian: Path

    @classmethod
    def setUpClass(cls) -> None:
        """Link the production owner; only open and VT ioctls are substituted."""
        temporary = tempfile.TemporaryDirectory(prefix="fplinux-drm-guardian-")
        cls.addClassCleanup(temporary.cleanup)
        cls.executable = Path(temporary.name) / "fplinux-vttest"
        cls.guardian = Path(temporary.name) / "vt-guardian"
        run_process(
            [
                "cc",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(ROOT / "alpine/aports/fplinux-base/fplinux-vt-guardian.c"),
                str(ROOT / "tests/host_tool/display/fplinux-drm-vt-fake.c"),
                "-Wl,--wrap=ioctl",
                "-o",
                str(cls.guardian),
            ],
            name="compile VT guardian host helper",
            timeout=30,
            check=True,
        )
        flags = shlex.split(
            run_process(
                ["pkg-config", "--cflags", "--libs", "libdrm"],
                name="read DRM compiler and linker flags",
                timeout=10,
                check=True,
            ).stdout
        )
        run_process(
            [
                "cc",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include/fplinux'}",
                str(ROOT / "tests/host_tool/display/fplinux-drm-vt.c"),
                str(ROOT / "tests/host_tool/display/fplinux-drm-vt-fake.c"),
                str(ROOT / "lib/fplinux/fplinux-drm-session.c"),
                "-Wl,--wrap=open,--wrap=ioctl,--wrap=execv",
                *flags,
                "-o",
                str(cls.executable),
            ],
            name="compile DRM VT guardian host harness",
            timeout=30,
            check=True,
        )

    def test_failed_open_and_process_death_restore_previous_vt(self) -> None:
        """Cleanup, process death and name-based kill restore the previous VT."""
        for mode in ("normal", "segv", "kill", "killall", "exec-fail"):
            with self.subTest(mode=mode):
                run_process(
                    [str(self.executable), mode, str(self.guardian)],
                    name=f"exercise DRM guardian {mode}",
                    timeout=10,
                    check=True,
                )


if __name__ == "__main__":
    unittest.main()
