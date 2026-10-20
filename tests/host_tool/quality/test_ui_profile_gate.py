# SPDX-License-Identifier: GPL-2.0-only
"""Check the delivered C profile table against its editable token document."""

from __future__ import annotations

import json
import shutil
import subprocess
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.quality import source as source_check
from fplinux_cli.reporting.run import RunReporter

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path


def test_c_scope_rejects_stale_profile_table_without_rewriting_it(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capfd: pytest.CaptureFixture[str]
) -> None:
    """An ordinary token edit must fail the gate until the delivered C is regenerated."""
    for relative in (
        "scripts/fplinux_ui_profile.py",
        "lib/fplinux/fplinux-ui-profile.json",
        "lib/fplinux/fplinux-ui-profile.c",
    ):
        destination = tmp_path / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / relative, destination)

    monkeypatch.setattr(source_check, "ROOT", tmp_path)
    monkeypatch.setattr("sys.argv", ["source-check", "c"])
    # Formatting and C analysis have their own checks; keep this fixture focused
    # on the real profile checker and its C scope dispatch.
    monkeypatch.setattr(source_check, "check_canonical_sources", lambda *_args: None)
    monkeypatch.setattr(source_check, "userspace_c_sources", lambda _files: [])
    monkeypatch.setattr(source_check, "check_userspace_c", lambda _sources: None)
    monkeypatch.setattr(RunReporter, "from_environment", lambda *_args: None)
    source_check.main()

    source = tmp_path / "lib/fplinux/fplinux-ui-profile.json"
    delivered = tmp_path / "lib/fplinux/fplinux-ui-profile.c"
    original_table = delivered.read_bytes()
    tokens = json.loads(source.read_text(encoding="utf-8"))
    tokens["profiles"]["128x128"]["margin"] = 9
    source.write_text(json.dumps(tokens), encoding="utf-8")

    with pytest.raises(subprocess.CalledProcessError) as error:
        source_check.main()
    assert error.value.returncode == 1
    assert "lib/fplinux/fplinux-ui-profile.c is stale" in capfd.readouterr().err
    assert delivered.read_bytes() == original_table

    source_check.run(["python3", "scripts/fplinux_ui_profile.py"])
    source_check.main()
