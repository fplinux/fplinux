# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the display services' startup hooks with controlled OpenRC state."""

from __future__ import annotations

import subprocess

import pytest

from tests import ROOT

START_PRE = ROOT / "tests/host_process/rootfs/fixtures/display-service/start-pre.sh"


@pytest.mark.parametrize(
    ("service", "other_service"),
    [
        ("fplinux-session", "fplinux-terminal"),
        ("fplinux-terminal", "fplinux-session"),
    ],
    ids=["graphical-session", "recovery-terminal"],
)
def test_display_startup_requires_the_other_owner_to_be_stopped(
    service: str, other_service: str
) -> None:
    """Both real hooks reject the active display owner and allow independent SSH."""
    script = ROOT / "alpine/aports" / service / f"{service}.initd"
    rejected = subprocess.run(
        ["sh", str(START_PRE), str(script), other_service],
        capture_output=True,
        text=True,
        check=False,
        timeout=5,
    )
    assert rejected.returncode == 1
    assert rejected.stdout == ""
    assert f"Stop {other_service} before starting" in rejected.stderr

    allowed = subprocess.run(
        ["sh", str(START_PRE), str(script), "fplinux-ssh"],
        capture_output=True,
        text=True,
        check=False,
        timeout=5,
    )
    assert allowed.returncode == 0, allowed.stderr
    assert allowed.stdout == "startup allowed\n"
    assert allowed.stderr == ""
