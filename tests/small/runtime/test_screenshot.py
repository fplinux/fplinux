# SPDX-License-Identifier: GPL-2.0-only
"""Exercise screenshot validation and local publication through the live console."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.runtime.live import console_connection

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path
    from typing import BinaryIO


_FRAME = b"P6\n2 1\n255\n\xff\x00\x0a\x00\xff\x00"


class Phone:
    """Provide saved-session authentication and remote output at the SSH boundary."""

    def __init__(self, frame: bytes, *, error: str | None) -> None:
        """Supply the response bytes and any subsequent SSH failure."""
        self.frame = frame
        self.error = error
        self.authenticated = False

    def reacquire_bound_session(
        self, session: dict[str, Any], *, expected_device_identity: str | None
    ) -> dict[str, Any]:
        """Retain the selected diagnostic connection."""
        assert expected_device_identity is None
        self.authenticated = True
        return session

    def run_remote(
        self, session: dict[str, Any], command: str, **_options: object
    ) -> subprocess.CompletedProcess[str]:
        """Return the running build metadata after authentication."""
        assert self.authenticated
        assert session["target"] == "phone"
        return subprocess.CompletedProcess(command, 0, "6.18.42-test\nbuild\n", "")

    def stream_remote(
        self,
        session: dict[str, Any],
        command: str,
        destination: BinaryIO,
        *,
        timeout: float,
        progress: Callable[[int], None],
    ) -> None:
        """Deliver the binary response over the bound command channel."""
        assert self.authenticated
        assert session["target"] == "phone"
        assert command == "exec /usr/libexec/fplinux/screenshot"
        assert timeout > 0
        destination.write(self.frame)
        progress(len(self.frame))
        if self.error:
            raise SystemExit(self.error)


def test_console_saves_complete_binary_ppm(tmp_path: Path) -> None:
    path = tmp_path / "screen.ppm"
    path.write_bytes(b"earlier screenshot")

    status = console_connection(
        (Phone(_FRAME, error=None), {"target": "phone"}),
        tmp_path / "data",
        tmp_path / "logs",
        admitted=None,
        screenshot=path,
    )

    assert status == 0
    assert path.read_bytes() == _FRAME
    assert {child.name for child in tmp_path.iterdir()} == {"data", "logs", "screen.ppm"}


@pytest.mark.parametrize(
    ("data", "error", "diagnostic"),
    [
        pytest.param(_FRAME, "Unsupported DRM scanout", "Unsupported DRM", id="remote-error"),
        pytest.param(b"remote banner\n" + _FRAME, None, "P6 PPM screenshot", id="invalid-header"),
        pytest.param(b"P6\n1025 1\n255\n", None, "screenshot dimensions", id="dimensions"),
        pytest.param(_FRAME[:-1], None, "incomplete or invalid", id="truncated"),
        pytest.param(_FRAME + b"\x00", None, "incomplete or invalid", id="trailing-byte"),
        pytest.param(b"x" * 3_200_000, None, "PPM size limit", id="oversize"),
    ],
)
def test_failed_console_screenshot_preserves_destination(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    data: bytes,
    error: str | None,
    diagnostic: str,
) -> None:
    output = tmp_path / "screen.ppm"
    output.write_bytes(b"earlier screenshot")

    status = console_connection(
        (Phone(data, error=error), {"target": "phone"}),
        tmp_path / "data",
        tmp_path / "logs",
        admitted=None,
        screenshot=output,
    )

    assert status == 1
    assert output.read_bytes() == b"earlier screenshot"
    assert {child.name for child in tmp_path.iterdir()} == {"data", "logs", "screen.ppm"}
    assert diagnostic in capsys.readouterr().err


def test_console_cleans_up_when_screenshot_cannot_replace_destination(tmp_path: Path) -> None:
    path = tmp_path / "screen.ppm"
    path.mkdir()
    (path / "keep").write_bytes(b"existing directory contents")

    status = console_connection(
        (Phone(_FRAME, error=None), {"target": "phone"}),
        tmp_path / "data",
        tmp_path / "logs",
        admitted=None,
        screenshot=path,
    )

    assert status == 1
    assert (path / "keep").read_bytes() == b"existing directory contents"
    assert {child.name for child in tmp_path.iterdir()} == {"data", "logs", "screen.ppm"}
