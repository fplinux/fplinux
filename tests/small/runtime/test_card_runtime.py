# SPDX-License-Identifier: GPL-2.0-only
"""Exercise card consent and reuse at the runtime's authenticated reader boundary."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import shlex
import subprocess
from contextlib import nullcontext
from types import SimpleNamespace
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.runtime import card, card_access
from fplinux_cli.runtime.ssh import commands

if TYPE_CHECKING:
    from pathlib import Path

CID = "f" * 32
TARGET = "inoi-240-modern-4g"
RELEASE = "123456789abc"
MIB = 1024**2
HOST_KEY = "AAAAC3NzaC1lZDI1NTE5AAAAIHJycnJycnJycnJycnJycnJycnJycnJycnJycnJycnJy"
CLIENT_KEY = b"AAAAC3NzaC1lZDI1NTE5AAAAIGtra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tr"


@pytest.fixture
def bundle(tmp_path: Path) -> Path:
    """Create an inventoried kit with a bounded card layout."""
    kit = tmp_path / "card"
    kit.mkdir()
    layout = {
        "target": TARGET,
        "release_id": RELEASE,
        "boot_offset": MIB,
        "boot_size": MIB,
        "root_offset": 2 * MIB,
        "root_size": MIB,
        "boot_sha256": "a" * 64,
        "root_sha256": "b" * 64,
        "fit_filename": "FPLINUX.ITB",
        "fit_ramdisk_offset": 64,
        "fit_ramdisk_size": 4096,
        "fit_hash_offset": 4192,
    }
    members = dict.fromkeys(("boot.xz", "root.ext4.xz", "board.tar", "fit.itb"), b"staged asset")
    members["layout.json"] = json.dumps(layout).encode()
    records = {}
    for name, contents in members.items():
        (kit / name).write_bytes(contents)
        records[f"card/{name}"] = {
            "size": len(contents),
            "sha256": hashlib.sha256(contents).hexdigest(),
        }
    (tmp_path / "build-manifest.json").write_text(
        json.dumps({"target": TARGET, "release_id": RELEASE, "files": records}), encoding="utf-8"
    )
    return tmp_path


def test_card_kit_damage_fails_before_opening_reader(
    bundle: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    (bundle / "card/board.tar").write_bytes(b"bad package bytes")
    monkeypatch.setattr(card, "run_phone", lambda *_args, **_kwargs: pytest.fail("started reader"))

    with pytest.raises(SystemExit, match=r"differs from its build manifest: board\.tar"):
        card.run_card(bundle, bundle / "data", bundle / "logs", erase_card=CID)


@pytest.mark.parametrize("inspect_only", [True, False])
def test_card_actions_use_one_reader_and_require_the_confirmed_cid(
    bundle: Path, monkeypatch: pytest.MonkeyPatch, *, inspect_only: bool
) -> None:
    session = {"session": "authenticated reader"}
    listing = SimpleNamespace(returncode=0, stdout=f"/dev/mmcblk0 {CID} 8192\n")
    log = SimpleNamespace(message=lambda _message: None, capture=nullcontext)
    progress_commands: list[str] = []

    def remote(_session: object, command: str, **_kwargs: object) -> SimpleNamespace:
        if command.startswith("/usr/libexec/fplinux/preparation "):
            progress_commands.append(command)
        return listing

    transport = SimpleNamespace(run_remote=remote)
    firmware: dict[str, tuple[bytes, ...]] = {"audio-profile": (b"captured data",)}
    visits = []

    def write(
        connection: tuple[object, object], selected: card.SdCard, *_args: object, **_kwargs: object
    ) -> None:
        visits.append(selected.cid)
        assert connection == (transport, session)
        assert selected.device == "/dev/mmcblk0"

    def start_reader(*_args: Any, **options: Any) -> int:  # noqa: ANN401 -- workflow peer.
        result = options["install_card"]((transport, session), firmware, log)
        assert result is not inspect_only
        return 0

    monkeypatch.setattr(card, "_write_card", write)
    monkeypatch.setattr(card, "run_phone", start_reader)
    result = card.run_card(
        bundle,
        bundle / "data",
        bundle / "logs",
        inspect_only=inspect_only,
        erase_card=None if inspect_only else CID,
    )

    assert result == 0
    assert visits == ([] if inspect_only else [CID])
    assert progress_commands == (
        []
        if inspect_only
        else [
            "/usr/libexec/fplinux/preparation --stage card-sending /run/fplinux-preparation-status"
        ]
    )


@pytest.mark.parametrize("outcome", ["invalid-host-key", "interrupted", "verified"])
def test_card_enrollment_uses_reader_pin_and_preserves_keys_once_writing_starts(
    bundle: Path, monkeypatch: pytest.MonkeyPatch, outcome: str
) -> None:
    messages: list[str] = []
    uploaded: dict[str, bytes] = {}
    progress_commands: list[str] = []
    remote_staging = "/tmp/fplinux-card.cancelled"  # noqa: S108 -- fake remote mktemp result.
    writer_started = False
    remote_cleaned = False
    store = bundle / "user data"
    credentials: Path | None = None
    monkeypatch.setenv("XDG_DATA_HOME", str(store))
    monkeypatch.setattr(
        card_access, "host_route_networks", lambda: {ipaddress.IPv4Network("10.0.0.0/8")}
    )
    monkeypatch.setattr(commands, "require_tool", lambda name: f"/native/{name}")

    def keygen(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[bytes]:
        assert argv[:2] == ["/native/ssh-keygen", "-q"]
        key = bundle.joinpath(argv[-1])
        key.write_bytes(b"installer PC private key")
        key.with_suffix(".pub").write_bytes(b"ssh-ed25519 " + CLIENT_KEY + b" fixture\n")
        return subprocess.CompletedProcess(argv, 0)

    monkeypatch.setattr(subprocess, "run", keygen)

    def media(_card_dir: Path, local: Path, *_args: object) -> tuple[Path, ...]:
        generated = tuple(local / name for name in ("personal.tar", "installed.sha256", "fit.itb"))
        for path in generated:
            path.write_bytes(b"test payload")
        return generated

    def remote(_session: object, command: str, **_kwargs: object) -> SimpleNamespace:
        nonlocal credentials, remote_cleaned, writer_started
        if command.startswith("mktemp "):
            return SimpleNamespace(returncode=0, stdout=f"{remote_staging}\n")
        if command.startswith("/usr/libexec/fplinux/preparation "):
            progress_commands.append(command)
            return SimpleNamespace(returncode=0, stdout="")
        if command.startswith(f"sh {remote_staging}/"):
            arguments = shlex.split(command)
            if arguments[2] == "prepare-access":
                return SimpleNamespace(
                    returncode=0,
                    stdout="unavailable\n" if outcome == "invalid-host-key" else HOST_KEY + "\n",
                )
            writer_started = True
            assert arguments[2:5] == ["/dev/mmcblk0", CID, remote_staging]
            assert arguments[-1] == HOST_KEY
            device_id = uploaded[f"{remote_staging}/device-id"]
            assert len(device_id) == 32
            serial = hashlib.sha256(device_id).hexdigest()[:32]
            usb = uploaded[f"{remote_staging}/usb-network"]
            assert len(usb) == 256
            network_fields = dict(
                line.split("=") for line in usb.rstrip(b"\0").decode().splitlines()
            )
            assert network_fields["usb_serial"] == serial
            assert network_fields["transport"] == "ncm"
            assert network_fields["netmask"] == "255.255.255.252"
            assert not ipaddress.IPv4Address(network_fields["host_address"]).is_global
            assert not network_fields["host_address"].startswith("10.")
            assert uploaded[f"{remote_staging}/client-key"] == CLIENT_KEY
            assert b"installer PC private key" not in uploaded.values()
            credentials = store / "fplinux/cards" / TARGET / CID / serial
            assert credentials.stat().st_mode & 0o777 == 0o700
            assert (
                credentials / "known_hosts"
            ).read_text() == f"fplinux-{serial} ssh-ed25519 {HOST_KEY}\n"
            assert (credentials / "id_ed25519").stat().st_mode & 0o777 == 0o600
            config = (credentials / "ssh_config").read_text()
            assert f'HostName "{network_fields["phone_address"]}"' in config
            assert f'BindAddress "{network_fields["host_address"]}"' in config
            assert 'StrictHostKeyChecking "yes"' in config
            if outcome == "interrupted":
                raise KeyboardInterrupt
            return SimpleNamespace(returncode=0, stderr="")
        if command.startswith("rm -f "):
            remote_cleaned = True
            return SimpleNamespace(returncode=0)
        if "mmcblk" in command:
            return SimpleNamespace(returncode=0, stdout=f"/dev/mmcblk0 {CID} 8192\n")
        pytest.fail(f"unexpected remote action: {command}")

    log = SimpleNamespace(
        message=messages.append,
        path=bundle / "runtime.log",
        capture=nullcontext,
        record=messages.append,
    )
    session = {"network": "10.23.45.0/30"}

    def upload(_session: object, local: str, remote: str) -> None:
        path = bundle.joinpath(local)
        uploaded[remote] = path.read_bytes()

    transport = SimpleNamespace(
        run_remote=remote,
        upload=upload,
    )

    def start_reader(*_args: Any, **options: Any) -> int:  # noqa: ANN401 -- workflow peer.
        assert options["install_card"]((transport, session), {}, log)
        return 0

    monkeypatch.setattr(card, "prepare_card_media", media)
    monkeypatch.setattr(card, "run_phone", start_reader)
    if outcome == "invalid-host-key":
        with pytest.raises(SystemExit, match="valid card SSH host key"):
            card.run_card(bundle, bundle / "data", bundle / "logs", erase_card=CID)
    elif outcome == "interrupted":
        with pytest.raises(KeyboardInterrupt):
            card.run_card(bundle, bundle / "data", bundle / "logs", erase_card=CID)
    else:
        assert card.run_card(bundle, bundle / "data", bundle / "logs", erase_card=CID) == 0

    assert f"{remote_staging}/fit.itb" in uploaded
    assert writer_started == (outcome != "invalid-host-key")
    assert remote_cleaned == (outcome != "interrupted")
    assert not any("installer PC private key" in message for message in messages)
    if outcome == "invalid-host-key":
        assert not list(store.rglob("id_ed25519"))
        return
    assert credentials is not None
    assert (credentials / "ssh_config").exists()
    if outcome == "interrupted":
        assert any("Keep the phone powered" in message for message in messages)
        assert [command.split()[2] for command in progress_commands] == [
            "card-sending",
            "cancelled",
        ]
    else:
        command = next(message for message in messages if message.startswith("/native/ssh "))
        assert shlex.split(command) == [
            "/native/ssh",
            "-F",
            str(credentials / "ssh_config"),
            "fplinux",
        ]
