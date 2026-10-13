# SPDX-License-Identifier: GPL-2.0-only
"""Exercise guardian process cleanup with a substituted VT ioctl boundary."""

from __future__ import annotations

import shlex
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@dataclass(frozen=True)
class GuardianHarness:
    """Linked VT client and the guardian process it executes."""

    executable: Path
    guardian: Path


class DrmVtGuardianTests:
    """Check process lifetime and return requests without claiming a live VT."""

    @pytest.fixture(scope="class")
    @classmethod
    def harness(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[GuardianHarness]:
        """Link the production owner; only open and VT ioctls are substituted."""
        with c_build_directory(tmp_path_factory, "fplinux-drm-vt") as temporary:
            executable = temporary / "fplinux-vttest"
            guardian = temporary / "vt-guardian"
            compile_c(
                guardian,
                [
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(ROOT / "alpine/aports/fplinux-base/fplinux-vt-guardian.c"),
                    str(ROOT / "tests/host_tool/display/fplinux-drm-vt-fake.c"),
                    "-Wl,--wrap=ioctl",
                ],
                name="compile VT guardian host helper",
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
            compile_c(
                executable,
                [
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
                ],
                name="compile DRM VT guardian host harness",
                check=True,
            )
            yield GuardianHarness(executable, guardian)

    @pytest.mark.parametrize(
        "mode",
        ["normal", "segv", "kill", "killall", "exec-fail"],
        ids=["normal", "segv", "kill", "killall", "exec-fail"],
    )
    def test_failed_open_and_process_death_restore_previous_vt(
        self, harness: GuardianHarness, mode: str
    ) -> None:
        """Cleanup, process death and name-based kill restore the previous VT."""
        run_process(
            [str(harness.executable), mode, str(harness.guardian)],
            name=f"exercise DRM guardian {mode}",
            timeout=10,
            check=True,
        )
