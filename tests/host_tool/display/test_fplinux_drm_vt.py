# SPDX-License-Identifier: GPL-2.0-only
"""Exercise guardian process cleanup with a substituted VT ioctl boundary."""

from __future__ import annotations

import shlex
import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


class DrmVtGuardianTests:
    """Check process lifetime and return requests without claiming a live VT."""

    executable: Path
    guardian: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the production owner; only open and VT ioctls are substituted."""
        with ExitStack() as cleanup:
            temporary = tempfile.TemporaryDirectory(prefix="fplinux-drm-guardian-")
            cleanup.enter_context(temporary)
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
            yield

    @pytest.mark.parametrize(
        "mode",
        ["normal", "segv", "kill", "killall", "exec-fail"],
        ids=["normal", "segv", "kill", "killall", "exec-fail"],
    )
    def test_failed_open_and_process_death_restore_previous_vt(self, mode: str) -> None:
        """Cleanup, process death and name-based kill restore the previous VT."""
        run_process(
            [str(self.executable), mode, str(self.guardian)],
            name=f"exercise DRM guardian {mode}",
            timeout=10,
            check=True,
        )
