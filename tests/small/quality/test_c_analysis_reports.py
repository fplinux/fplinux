# SPDX-License-Identifier: GPL-2.0-only
"""Retain native analyzer evidence in the public command's log directory."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.quality import userspace_c

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize("failed", [False, True], ids=["success", "analyzer-failure"])
def test_command_logs_retain_c_reports_after_success_and_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, *, failed: bool
) -> None:
    log_root = tmp_path / "logs"
    monkeypatch.delenv("FPLINUX_SCAN_BUILD_DIR", raising=False)
    monkeypatch.setenv("FPLINUX_LOG_ROOT", str(log_root))

    def analyze(output: Path, sources: list[tuple[str, bool]]) -> None:
        assert sources == [("native.c", False)]
        output.mkdir(parents=True)
        (output / "report.html").write_text("native analysis evidence\n", encoding="utf-8")
        if failed:
            message = "native analyzer reported a defect"
            raise RuntimeError(message)

    monkeypatch.setattr(userspace_c, "run_userspace_analysis", analyze)
    if failed:
        with pytest.raises(RuntimeError, match="native analyzer reported a defect"):
            userspace_c.check_userspace_c([("native.c", False)])
    else:
        userspace_c.check_userspace_c([("native.c", False)])

    assert (log_root / "c-analysis/report.html").read_bytes() == b"native analysis evidence\n"
