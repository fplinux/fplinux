# SPDX-License-Identifier: GPL-2.0-only
"""Exercise DRM display selection and guardian cleanup with synthetic devices."""

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
        """Link the production session with synthetic DRM queries and VT ioctls."""
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
                    str(ROOT / "tests/host_tool/display/fplinux-drm-selection-fake.c"),
                    "-Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=execv",
                    "-Wl,--wrap=drmGetDevices2,--wrap=drmFreeDevices",
                    "-Wl,--wrap=drmSetClientCap,--wrap=drmSetMaster",
                    "-Wl,--wrap=drmModeGetResources,--wrap=drmModeFreeResources",
                    "-Wl,--wrap=drmModeGetConnector,--wrap=drmModeGetConnectorCurrent",
                    "-Wl,--wrap=drmModeFreeConnector",
                    "-Wl,--wrap=drmModeGetEncoder,--wrap=drmModeFreeEncoder",
                    "-Wl,--wrap=drmModeGetPlaneResources,--wrap=drmModeFreePlaneResources",
                    "-Wl,--wrap=drmModeGetPlane,--wrap=drmModeFreePlane",
                    "-Wl,--wrap=drmModeObjectGetProperties,--wrap=drmModeFreeObjectProperties",
                    "-Wl,--wrap=drmModeGetProperty,--wrap=drmModeFreeProperty",
                    *flags,
                ],
                name="compile DRM VT guardian host harness",
                check=True,
            )
            yield GuardianHarness(executable, guardian)

    @pytest.fixture(scope="class")
    @classmethod
    def handoff_executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
        """Link real acquisition/dispatch with synthetic VT and DRM ownership."""
        with c_build_directory(tmp_path_factory, "fplinux-drm-handoff") as temporary:
            executable = temporary / "drm-handoff"
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
                    str(ROOT / "tests/host_tool/display/fplinux-drm-handoff.c"),
                    str(ROOT / "lib/fplinux/fplinux-drm-session.c"),
                    "-Wl,--wrap=open,--wrap=execv,--wrap=ioctl,--wrap=drmIoctl",
                    "-Wl,--wrap=drmSetClientCap,--wrap=drmSetMaster,--wrap=drmDropMaster",
                    "-Wl,--wrap=drmModeGetResources,--wrap=drmModeFreeResources",
                    "-Wl,--wrap=drmModeGetConnector,--wrap=drmModeFreeConnector",
                    "-Wl,--wrap=drmModeGetEncoder,--wrap=drmModeFreeEncoder",
                    "-Wl,--wrap=drmModeGetPlaneResources,--wrap=drmModeFreePlaneResources",
                    "-Wl,--wrap=drmModeGetPlane,--wrap=drmModeFreePlane",
                    "-Wl,--wrap=drmModeObjectGetProperties,--wrap=drmModeFreeObjectProperties",
                    "-Wl,--wrap=drmModeGetProperty,--wrap=drmModeFreeProperty",
                    "-Wl,--wrap=drmModeCreatePropertyBlob,--wrap=drmModeDestroyPropertyBlob",
                    "-Wl,--wrap=drmModeAddFB2,--wrap=drmModeRmFB",
                    *flags,
                ],
                name="compile DRM acquisition and dispatch with synthetic devices",
                check=True,
            )
            yield executable

    @pytest.mark.parametrize("mode", ["initial-acquire", "already-active", "pending-release"])
    def test_first_dispatch_preserves_a_release_queued_during_initialization(
        self, harness: GuardianHarness, handoff_executable: Path, mode: str
    ) -> None:
        """A pending release hands DRM to the terminal and permits a later acquire."""
        run_process(
            [str(handoff_executable), str(harness.guardian), mode],
            name=f"exercise DRM ownership after initialization: {mode}",
            timeout=10,
            check=True,
        )

    @pytest.mark.parametrize(
        "mode",
        ["normal", "abort", "kill", "killall", "exec-fail"],
        ids=["normal", "abort", "kill", "killall", "exec-fail"],
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

    @pytest.mark.parametrize(
        "mode",
        [
            "gpu-panel",
            "cold-panel",
            "panel-card7",
            "ambiguous",
            "no-panel",
            "explicit-gpu",
            "explicit-panel",
            "explicit-missing",
            "nv16-panel",
            "nv16-unsupported",
            "probe-error",
        ],
    )
    def test_display_selection_uses_unique_compatible_kms_device(
        self, harness: GuardianHarness, mode: str
    ) -> None:
        """Synthetic DRM devices exercise selection and failed-open VT cleanup."""
        run_process(
            [str(harness.executable), mode, str(harness.guardian)],
            name=f"exercise DRM display selection {mode}",
            timeout=10,
            check=True,
        )
