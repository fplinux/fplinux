# SPDX-License-Identifier: GPL-2.0-only
"""Phone access setup preserves local rules and handles the privileged host boundary."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest
from fplinux_cli.runtime import boot as runner


def test_existing_local_rules_are_preserved_without_privileged_commands(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Previously installed or locally customized rules need no new setup."""
    destination = tmp_path / "etc/60-fplinux.rules"
    destination.parent.mkdir()
    destination.write_bytes(b"local phone access policy\n")
    monkeypatch.setattr(runner, "USB_RULES_FILE", destination)

    def reject_command(*_arguments: object, **_options: object) -> None:
        pytest.fail("existing local rules must not invoke a privileged command")

    monkeypatch.setattr(subprocess, "run", reject_command)
    runner.prepare_usb_access(tmp_path)
    assert destination.read_bytes() == b"local phone access policy\n"


def test_new_rules_are_installed_before_udev_reload(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A controlled install boundary writes real rules and reloads only after success."""
    bundle = tmp_path / "archive"
    bundle.mkdir()
    (bundle / "60-fplinux.rules").write_bytes(b"phone access rules\n")
    destination = tmp_path / "etc/60-fplinux.rules"
    destination.parent.mkdir()
    monkeypatch.setattr(runner, "USB_RULES_FILE", destination)
    monkeypatch.setattr(shutil, "which", lambda name: f"/usr/bin/{name}")
    effects: list[str] = []

    def host_command(argv: list[str], *, check: bool) -> subprocess.CompletedProcess[bytes]:
        """Replace sudo and udev, leaving the installed file observable."""
        assert not check
        if argv[1] == "install":
            assert argv[2:4] == ["-m", "0644"]
            shutil.copyfile(argv[-2], argv[-1])
            Path(argv[-1]).chmod(0o644)
            effects.append("installed")
        else:
            assert destination.read_bytes() == b"phone access rules\n"
            assert argv[1:] == ["/usr/bin/udevadm", "control", "--reload-rules"]
            effects.append("reloaded")
        return subprocess.CompletedProcess(argv, 0)

    monkeypatch.setattr(subprocess, "run", host_command)
    runner.prepare_usb_access(bundle)
    assert effects == ["installed", "reloaded"]
    assert destination.stat().st_mode & 0o777 == 0o644


def test_declined_privileged_install_does_not_reload_udev(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A refused setup stops before the next privileged host operation."""
    (tmp_path / "60-fplinux.rules").write_bytes(b"phone rules\n")
    monkeypatch.setattr(runner, "USB_RULES_FILE", tmp_path / "missing-rules")
    monkeypatch.setattr(shutil, "which", lambda name: f"/usr/bin/{name}")
    attempted: list[list[str]] = []

    def refused(argv: list[str], *, check: bool) -> subprocess.CompletedProcess[bytes]:
        assert not check
        attempted.append(argv)
        return subprocess.CompletedProcess(argv, 1)

    monkeypatch.setattr(subprocess, "run", refused)
    with pytest.raises(SystemExit, match="USB access setup was not completed"):
        runner.prepare_usb_access(tmp_path)
    assert len(attempted) == 1
    assert attempted[0][1] == "install"
