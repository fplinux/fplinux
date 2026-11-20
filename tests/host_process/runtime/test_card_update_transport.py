# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the authenticated command and generated binary transfer with a local SSH peer."""

from __future__ import annotations

import json
import shlex
from pathlib import Path
from shutil import copyfile
from subprocess import CompletedProcess as ProcessResult
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.runtime import card_update
from fplinux_cli.runtime.card import SdCard
from fplinux_cli.runtime.card_media import read_card_layout
from fplinux_cli.runtime.events import RunLog
from fplinux_cli.runtime.ssh import transport

if TYPE_CHECKING:
    from collections.abc import Iterator

    from fplinux_cli.runtime.card_media import CardLayout

_PEER = Path(__file__).parent / "fixtures/card-update/ssh-peer.sh"
_REMOTE = "/tmp/fplinux-card-update.TEST0123"  # noqa: S108 -- fixture reports a remote path.
_CARD = SdCard(device="/dev/mmcblk1", cid="0" * 32, bytes=32 * 1024**2)


@pytest.fixture
def layout(tmp_path: Path) -> CardLayout:
    """Describe a fixed FAT partition and the minimum system partition size."""
    record = {
        "target": "example-target",
        "release_id": "222222222222",
        "disk_signature": 0x46504C58,
        "boot_offset": 1048576,
        "boot_size": 4194304,
        "root_offset": 5242880,
        "root_size": 4194304,
        "boot_sha256": "a" * 64,
        "root_sha256": "b" * 64,
        "fit_filename": "FPLINUX.ITB",
        "fit_ramdisk_offset": 40,
        "fit_ramdisk_size": 5120,
        "fit_hash_offset": 80,
    }
    (tmp_path / "layout.json").write_text(json.dumps(record), encoding="ascii")
    return read_card_layout(tmp_path)


@pytest.fixture
def log(tmp_path: Path) -> Iterator[RunLog]:
    """Retain user warnings and peer diagnostics in the normal runtime log."""
    run = RunLog(tmp_path, protect=lambda path: path.chmod(0o700))
    yield run
    run.close("finished")


@pytest.fixture
def payload(tmp_path: Path) -> dict[str, Path]:
    """Separate the bulk payload from the local reader tool directory."""
    result = {}
    for name, data in (
        ("root.ext4.xz", bytes(range(256)) * 8192),
        ("board.tar", b"board archive"),
        ("board.sha256", b"board member checksums"),
        ("fit.itb", b"new system FIT"),
        ("apks/Packages.adb", b"release package inventory"),
        ("apks/fplinux-build.rsa.pub", b"release public key"),
        ("apks/example-1.apk", b"example release package"),
    ):
        path = tmp_path / name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(data)
        result[name] = path
    return result


@pytest.fixture
def remote(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Replace USB/session discovery and SSH with a local process, watching remote calls."""
    remote = tmp_path / "reader"
    remote.mkdir()
    tools = tmp_path / "tools"
    tools.mkdir()
    for variable, name in (
        ("CARD_SLOTS", "card-slots.sh"),
        ("CARD_UPDATE_STAGE", "update-card.sh"),
        ("CARD_APK_WORLD", "card-apk-world.sh"),
    ):
        script = tools / name
        script.write_bytes(b"controlled tool fixture\n")
        monkeypatch.setattr(card_update, variable, script)
    peer = tools / "ssh"
    copyfile(_PEER, peer)
    peer.chmod(0o700)

    def control(_session: dict[str, Any], command: str, **_kwargs: object) -> ProcessResult[str]:
        with (remote / "control").open("a", encoding="ascii") as stream:
            stream.write(f"{command}\n")
        assert command == "mktemp -d /tmp/fplinux-card-update.XXXXXXXX"
        return ProcessResult(command, 0, f"{_REMOTE}\n", "")

    def upload(_session: dict[str, Any], local: str, destination: str) -> None:
        assert destination == f"{_REMOTE}/{Path(local).name}"
        copyfile(local, remote / Path(local).name)

    def arguments(session: dict[str, Any], command: str) -> list[str]:
        return [str(peer), session["scenario"], str(remote), command]

    monkeypatch.setattr(transport, "validate_session", lambda session: session)
    monkeypatch.setattr(transport, "ssh_argv", arguments)
    monkeypatch.setattr(transport, "run_remote", control)
    monkeypatch.setattr(transport, "upload", upload)
    return remote


def test_update_sends_only_small_tools_then_streams_the_release_once(
    remote: Path, layout: CardLayout, payload: dict[str, Path], log: RunLog
) -> None:
    """One authenticated command receives the actual archive and its remote destination."""
    card_update.update_card(
        (transport, {"scenario": "transfer"}), _CARD, layout, assets=payload, log=log
    )
    assert {path.name for path in remote.iterdir()} == {
        "card-slots.sh",
        "card-update-reader.sh",
        "card-apk-world.sh",
        "update-card.sh",
        "transfer.sha256",
        "control",
        "command",
        "received",
    }
    for name, path in payload.items():
        assert (remote / "received" / name).read_bytes() == path.read_bytes()
    command = shlex.split((remote / "command").read_text(encoding="ascii"))
    assert command[:12] == [
        "sh",
        f"{_REMOTE}/card-update-reader.sh",
        "update",
        "/dev/mmcblk1",
        "0" * 32,
        "example-target",
        "222222222222",
        "1179667544",
        "2048",
        "8192",
        "10240",
        "8192",
    ]
    assert "was written and the card unmounted" in log.path.read_text(encoding="utf-8")


def test_update_accepts_early_reader_exit_for_an_already_selected_build(
    remote: Path, layout: CardLayout, payload: dict[str, Path], log: RunLog
) -> None:
    """A no-op closes SSH input without extracting the pending archive."""
    card_update.update_card(
        (transport, {"scenario": "already-active"}), _CARD, layout, assets=payload, log=log
    )
    assert not (remote / "received").exists()
    assert "is already selected" in log.path.read_text(encoding="utf-8")


def test_update_reports_disconnect_without_a_second_remote_command(
    remote: Path, layout: CardLayout, payload: dict[str, Path], log: RunLog
) -> None:
    """The host cannot prove remote unmount after SSH loss and leaves cleanup to the reader."""
    with pytest.raises(SystemExit, match="connection lost during input"):
        card_update.update_card(
            (transport, {"scenario": "disconnect"}), _CARD, layout, assets=payload, log=log
        )
    assert (remote / "control").read_text(encoding="ascii").splitlines() == [
        "mktemp -d /tmp/fplinux-card-update.XXXXXXXX",
    ]
    assert "may still be working on the SD card" in log.path.read_text(encoding="utf-8")
