# SPDX-License-Identifier: GPL-2.0-only
"""Observe the durable system selection and retained card files across operations."""

from __future__ import annotations

import hashlib
import shutil
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path
    from subprocess import CompletedProcess

_WRITER = ROOT / "scripts/fplinux_cli/runtime/card-slots.sh"
_UPDATE = ROOT / "scripts/fplinux_cli/runtime/update-card.sh"
_BUSYBOX = ROOT / "tests/host_process/runtime/fixtures/card-slots/busybox.sh"
_TARGET = "nokia-ta1618"
_A = "134567abcdef"
_B = "389abac7246e"


def _write_pair(card: Path, slot: str, build: str) -> None:
    directory = card / "system" / slot
    etc = directory / "root" / "etc"
    etc.mkdir(parents=True)
    (etc / "os-release").write_text(f"NAME=FPLinux\nBUILD_ID={build}\n", encoding="ascii")
    (directory / "FPLINUX.ITB").write_bytes(f"FIT for {build}".encode())


def _run(card: Path, action: str, build: str, *, target: str = _TARGET) -> CompletedProcess[str]:
    return run_process(
        ["sh", str(_WRITER), str(card), target, action, build],
        name=f"{action} a system card slot",
        timeout=10,
    )


@pytest.fixture
def installed_card(tmp_path: Path) -> Path:
    _write_pair(tmp_path, "a", _A)
    assert _run(tmp_path, "initialize", _A).returncode == 0
    assert (tmp_path / "system/target").read_bytes() == b"nokia-ta1618\n"
    assert (tmp_path / "system/active").read_bytes() == b"a\n"
    return tmp_path


def test_ready_pair_survives_update_and_rollback(installed_card: Path) -> None:
    card = installed_card
    files = card / "data/home/fplinux"
    files.mkdir(parents=True)
    saved = files / "test-note"
    saved.write_bytes(b"same note across system changes\n")
    old_fit = card / "system/a/FPLINUX.ITB"
    old_bytes = old_fit.read_bytes()
    _write_pair(card, "b", _B)
    active = card / "system/active"

    assert _run(card, "activate", _B).returncode == 1
    assert active.read_bytes() == b"a\n"
    assert _run(card, "confirm", _A).returncode == 0
    assert _run(card, "activate", _B).returncode == 0
    assert active.read_bytes() == b"b\n"
    selected_inode = active.stat().st_ino
    assert _run(card, "activate", _B).returncode == 0
    assert active.stat().st_ino == selected_inode
    assert _run(card, "activate", _A).returncode == 1
    assert _run(card, "confirm", _A).returncode == 1
    assert _run(card, "rollback", _B).returncode == 1
    assert active.read_bytes() == b"b\n"
    assert _run(card, "rollback", _A).returncode == 0
    assert _run(card, "rollback", _A).returncode == 0
    assert active.read_bytes() == b"a\n"
    assert _run(card, "rollback", _B).returncode == 1
    assert _run(card, "activate", _B).returncode == 0
    assert _run(card, "confirm", _B).returncode == 0
    assert _run(card, "rollback", _A).returncode == 0
    assert active.read_bytes() == b"a\n"
    assert saved.read_bytes() == b"same note across system changes\n"
    assert old_fit.read_bytes() == old_bytes


@pytest.mark.parametrize("invalid", [b"a", b"a\n\n", b"c\n"])
def test_invalid_selection_does_not_change_the_card(installed_card: Path, invalid: bytes) -> None:
    active = installed_card / "system/active"
    active.write_bytes(invalid)
    result = _run(installed_card, "confirm", _A)
    assert result.returncode == 1
    assert "active slot is invalid" in result.stderr
    assert active.read_bytes() == invalid
    assert not (installed_card / "system/a/ready").exists()


def test_wrong_target_and_incomplete_pair_fail_before_publication(installed_card: Path) -> None:
    card = installed_card
    assert _run(card, "confirm", _A, target="nokia-110-4g").returncode == 1
    assert not (card / "system/a/ready").exists()
    assert _run(card, "confirm", _A).returncode == 0
    _write_pair(card, "b", _B)
    other_fit = card / "system/b/FPLINUX.ITB"
    other_fit.unlink()
    other_fit.symlink_to("../a/FPLINUX.ITB")
    assert _run(card, "activate", _B).returncode == 1
    assert other_fit.is_symlink()
    assert (card / "system/active").read_bytes() == b"a\n"


def test_rollback_recovers_when_the_active_fit_is_missing(installed_card: Path) -> None:
    card = installed_card
    assert _run(card, "confirm", _A).returncode == 0
    _write_pair(card, "b", _B)
    assert _run(card, "activate", _B).returncode == 0
    (card / "system/b/FPLINUX.ITB").unlink()
    assert _run(card, "rollback", _A).returncode == 0
    assert (card / "system/active").read_bytes() == b"a\n"


def test_rollback_rejects_a_confirmed_fit_whose_bytes_changed(installed_card: Path) -> None:
    card = installed_card
    assert _run(card, "confirm", _A).returncode == 0
    _write_pair(card, "b", _B)
    assert _run(card, "activate", _B).returncode == 0
    (card / "system/a/FPLINUX.ITB").write_bytes(b"different FIT")
    result = _run(card, "rollback", _A)
    assert result.returncode == 1
    assert (card / "system/active").read_bytes() == b"b\n"


def test_update_replaces_only_the_inactive_slot_after_checking_candidate_identity(
    installed_card: Path,
) -> None:
    card = installed_card
    previous = "0123456789ab"
    _write_pair(card, "b", previous)
    _write_pair(card, ".update/pair", _B)
    old = card / "system/a"
    original_world = old / "root/etc/apk/world"
    original_world.parent.mkdir()
    original_world.write_bytes(b"alpine-base\n")
    (old / "base-world").write_bytes(b"alpine-base\n")
    staging = card / "system/.update"
    candidate = staging / "pair"
    (candidate / "base-world").write_bytes(b"alpine-base\n")
    (staging / "board.sha256").write_bytes(b"")
    fit_sha = hashlib.sha256(f"FIT for {_B}".encode()).hexdigest()
    board_sha = hashlib.sha256(b"").hexdigest()
    publish = ["sh", str(_UPDATE), "publish", str(card), _TARGET, _B, fit_sha, board_sha]
    active = card / "system/active"
    saved_fit = card / "system/b/FPLINUX.ITB"

    result = run_process(publish, name="refuse to reclaim a slot before confirmation", timeout=10)
    assert result.returncode == 1
    assert "Confirm that the active system boots" in result.stderr
    assert saved_fit.read_bytes() == b"FIT for 0123456789ab"
    assert active.read_bytes() == b"a\n"
    assert _run(card, "confirm", _A).returncode == 0

    release = candidate / "root/etc/os-release"
    release.write_bytes(b"BUILD_ID=0123456789ab\n")
    result = run_process(publish, name="reject a mismatched replacement root", timeout=10)
    assert result.returncode == 1
    assert "prepared pair belongs to another build" in result.stderr
    assert saved_fit.read_bytes() == b"FIT for 0123456789ab"
    assert active.read_bytes() == b"a\n"

    new_release = f"BUILD_ID={_B}\nFPLINUX_SYSTEM_CARD_PROFILE=microsd-uboot\n"
    release.write_text(new_release, encoding="ascii")
    result = run_process(publish, name="publish the prepared pair", timeout=10)
    assert result.returncode == 0, result.stderr
    assert active.read_bytes() == b"b\n"
    assert (card / "system/a/FPLINUX.ITB").read_bytes() == f"FIT for {_A}".encode()
    assert (card / "system/b/base-world").read_bytes() == b"alpine-base\n"
    assert (card / "system/b/root/etc/os-release").read_bytes() == new_release.encode("ascii")
    assert saved_fit.read_bytes() == f"FIT for {_B}".encode()
    assert not candidate.exists()

    shutil.rmtree(staging)
    selector_inode = active.stat().st_ino
    result = run_process(publish, name="retry the already selected update", timeout=10)
    assert result.returncode == 0, result.stderr
    assert active.stat().st_ino == selector_inode
    publish[-2] = "0" * 64
    result = run_process(publish, name="reject a different FIT for the selected build", timeout=10)
    assert result.returncode == 1
    assert "active build has a different personalized FIT" in result.stderr
    assert active.stat().st_ino == selector_inode


@pytest.mark.parametrize(("failed_flush", "selected"), [("1", b"a\n"), ("2", b"b\n")])
def test_failed_flush_leaves_a_complete_pair_and_can_be_retried(
    installed_card: Path,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    failed_flush: str,
    selected: bytes,
) -> None:
    card = installed_card
    assert _run(card, "confirm", _A).returncode == 0
    _write_pair(card, "b", _B)
    binaries = tmp_path / "bin"
    binaries.mkdir()
    wrapper = binaries / "busybox"
    shutil.copyfile(_BUSYBOX, wrapper)
    wrapper.chmod(0o755)
    monkeypatch.setenv("CARD_TEST_BUSYBOX", "/bin/busybox")
    monkeypatch.setenv("CARD_TEST_SYNC_LOG", str(tmp_path / "syncs"))
    monkeypatch.setenv("CARD_TEST_FAIL_SYNC", failed_flush)
    monkeypatch.setenv("PATH", str(binaries), prepend=":")

    result = _run(card, "activate", _B)
    assert result.returncode == 1
    assert "Cannot flush the system card" in result.stderr
    active = card / "system/active"
    assert active.read_bytes() == selected
    digest = hashlib.sha256(f"FIT for {_A}".encode()).hexdigest()
    assert (card / "system/a/ready").read_text() == f"{_A} {digest}\n"
    assert (card / "system/b/FPLINUX.ITB").read_bytes() == f"FIT for {_B}".encode()
    assert sorted(path.name for path in (card / "system").iterdir()) == [
        "a",
        "active",
        "b",
        "target",
    ]
    assert _run(card, "activate", _B).returncode == 0
    assert active.read_bytes() == b"b\n"
