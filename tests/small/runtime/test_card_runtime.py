# SPDX-License-Identifier: GPL-2.0-only
"""Exercise card consent and reuse at the runtime's authenticated reader boundary."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import shlex
import subprocess
import tarfile
from base64 import b64encode
from contextlib import nullcontext
from io import BytesIO
from types import SimpleNamespace
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.cli import card as checkout_card
from fplinux_cli.cli.dispatch import command_action
from fplinux_cli.cli.parser import create_parser
from fplinux_cli.device_data import inputs
from fplinux_cli.runtime import boot, card, card_access, phone_data, workflow
from fplinux_cli.runtime.bundle import ValidatedBundle
from fplinux_cli.runtime.ssh import commands

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path
    from typing import IO

CID = "f" * 32
TARGET = "inoi-240-modern-4g"
RELEASE = "123456789abc"
MIB = 1024**2
HOST_KEY = b64encode(b"\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20" + bytes(range(32))).decode()
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
        "disk_signature": 0x46504C58,
        "boot_size": 2 * MIB,
        "root_offset": 3 * MIB,
        "root_size": MIB,
        "boot_prefix_size": 2 * MIB,
        "boot_sha256": "a" * 64,
        "boot_file_sha256": "c" * 64,
        "root_sha256": "b" * 64,
        "fit_filename": "FPLINUX.ITB",
        "fit_ramdisk_offset": 64,
        "fit_ramdisk_size": 5120,
        "fit_hash_offset": 4192,
    }
    members = dict.fromkeys(
        (
            "boot.xz",
            "root.ext4.xz",
            "board.tar",
            "fit.itb",
            "apk-state.tar",
        ),
        b"staged asset",
    )
    members["layout.json"] = json.dumps(layout).encode()
    members["default-apps"] = b"fplinux-notes\n"
    members["apks/Packages.adb"] = b"signed index"
    members["apks/fplinux-build.rsa.pub"] = b"release public key"
    members["apks/fplinux-notes.apk"] = b"notes package"
    records = {}
    for name, contents in members.items():
        (kit / name).parent.mkdir(exist_ok=True)
        (kit / name).write_bytes(contents)
        records[f"card/{name}"] = {
            "size": len(contents),
            "sha256": hashlib.sha256(contents).hexdigest(),
        }
    (tmp_path / "build-manifest.json").write_text(
        json.dumps({"target": TARGET, "release_id": RELEASE, "files": records}), encoding="utf-8"
    )
    return tmp_path


@pytest.mark.parametrize(
    "asset",
    [
        "board.tar",
        "default-apps",
        "apk-state.tar",
        "apks/Packages.adb",
        "apks/fplinux-build.rsa.pub",
        "apks/fplinux-notes.apk",
    ],
)
def test_card_kit_damage_fails_before_opening_reader(
    bundle: Path, monkeypatch: pytest.MonkeyPatch, asset: str
) -> None:
    (bundle / "card" / asset).write_bytes(b"broken asset")
    monkeypatch.setattr(card, "run_phone", lambda *_args, **_kwargs: pytest.fail("started reader"))

    with pytest.raises(SystemExit, match="differs from its build manifest") as captured:
        card.run_card(bundle, bundle / "data", bundle / "logs", erase_card=CID)
    assert str(captured.value).endswith(asset)


def test_checkout_card_inspect_builds_and_resolves_only_the_ram_profile(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    parser, check_parser = create_parser((TARGET,))
    args = parser.parse_args(
        [
            "card",
            "inspect",
            TARGET,
            "--build-type",
            "debug",
            "--jobs",
            "2",
            "--from-dump",
            str(tmp_path / "nand.bin"),
            "--phone-data",
            str(tmp_path / "phone"),
        ]
    )
    ram = tmp_path / "ram"
    calls: list[str] = []

    def build(target: str, jobs: int, *, profile: str | None = None, **options: object) -> None:
        assert profile is None, "card inspect requested a system-card build"
        assert (target, jobs) == (TARGET, 2)
        assert options == {"build_type": "debug", "include_device_data": False}
        calls.append("build")

    def resolve(
        target: str, profile: str | None = None, *, build_type: str
    ) -> tuple[SimpleNamespace, dict[str, str]]:
        assert (target, profile, build_type) == (TARGET, None, "debug")
        calls.append("resolve")
        return SimpleNamespace(path=ram), {}

    def inspect(bundle: Path, data: Path, logs: Path, **options: object) -> int:
        assert bundle == ram
        assert data.is_relative_to(tmp_path)
        assert logs == tmp_path / "logs/run" / TARGET
        assert options["action"] == "inspect"
        assert options.get("card_bundle") is None
        assert options["from_dump"] == tmp_path / "nand.bin"
        assert options["phone_data"] == tmp_path / "phone"
        calls.append("inspect")
        return 0

    monkeypatch.setattr(checkout_card, "data_root", lambda _root: tmp_path)
    monkeypatch.setattr(checkout_card, "build", build)
    monkeypatch.setattr(checkout_card, "resolve_target_bundle", resolve)
    monkeypatch.setattr(card, "run_card", inspect)
    command_action(args, check_parser)()

    assert calls == ["build", "resolve", "inspect"]


@pytest.mark.parametrize("inspect_only", [True, False])
def test_card_actions_report_capacity_and_only_install_calls_writer(
    bundle: Path, monkeypatch: pytest.MonkeyPatch, *, inspect_only: bool
) -> None:
    if inspect_only:
        (bundle / "card/boot.xz").unlink()
    session = {"session": "authenticated reader"}
    listing = SimpleNamespace(returncode=0, stdout=f"/dev/mmcblk0 {CID} 8192\n")
    messages: list[str] = []
    log = SimpleNamespace(message=messages.append, capture=nullcontext)
    progress_commands: list[str] = []

    def remote(_session: object, command: str, **_kwargs: object) -> SimpleNamespace:
        if command.startswith("/usr/libexec/fplinux/preparation "):
            progress_commands.append(command)
        return listing

    transport = SimpleNamespace(run_remote=remote)
    visits: list[str] = []

    def write(
        connection: tuple[object, object], selected: card.SdCard, *_args: object, **_kwargs: object
    ) -> None:
        visits.append(selected.cid)
        assert connection == (transport, session)
        assert selected.device == "/dev/mmcblk0"

    def start_reader(*_args: Any, **options: Any) -> int:  # noqa: ANN401 -- workflow peer.
        if inspect_only:
            assert "install_card" not in options
            options["inspect_card"]((transport, session), log)
        else:
            assert "inspect_card" not in options
            options["install_card"]((transport, session), {}, log)
        return 0

    monkeypatch.setattr(card, "_write_card", write)
    monkeypatch.setattr(card, "run_phone", start_reader)
    result = card.run_card(
        bundle,
        bundle / "data",
        bundle / "logs",
        action="inspect" if inspect_only else "install",
        erase_card=None if inspect_only else CID,
    )

    assert result == 0
    expected_messages = [f"SD card {CID} (4 MiB) at /dev/mmcblk0"]
    if not inspect_only:
        expected_messages += [
            "Shared files (FAT32 partition): 2 MiB",
            "FPLinux (ext4 partition): 1 MiB initially, 1 MiB after first-boot expansion",
        ]
    assert messages == expected_messages
    assert visits == ([] if inspect_only else [CID])
    assert progress_commands == (
        []
        if inspect_only
        else [
            "/usr/libexec/fplinux/preparation --stage card-sending /run/fplinux-preparation-status"
        ]
    )


@pytest.mark.parametrize("reader_present", [True, False], ids=["same-reader", "reader-lost"])
def test_card_inspect_requires_the_retained_authenticated_reader(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    *,
    reader_present: bool,
) -> None:
    ram = tmp_path / "ram"
    data = tmp_path / "runtime data"
    store = tmp_path / "phone originals"
    admitted = ValidatedBundle(
        ram,
        {
            "target": TARGET,
            "identity": {"target": {"display_name": "Fixture phone"}},
            "profile": None,
            "build_type": "release",
        },
        "a" * 64,
        "b" * 64,
        None,
    )
    session = {"session": "authenticated reader"}
    calls: list[str] = []

    def prepare(*_args: object, **options: object) -> bool:
        assert options["store"] == store
        assert options["retain_reader"] is True
        calls.append("prepare")
        return True

    def remote(actual: object, command: str, **_kwargs: object) -> SimpleNamespace:
        assert actual is session
        assert "/sys/block/mmcblk" in command
        calls.append("list")
        return SimpleNamespace(returncode=0, stdout=f"/dev/mmcblk0 {CID} 8192\n")

    def reconnect(
        actual: ValidatedBundle, **options: object
    ) -> tuple[SimpleNamespace, dict[str, str]] | None:
        assert actual is admitted
        assert options["session_root"] == data / "reader/session"
        assert options["required"] is True
        calls.append("authenticate")
        return (SimpleNamespace(run_remote=remote), session) if reader_present else None

    monkeypatch.setattr(workflow, "host_preflight", lambda: None)
    monkeypatch.setattr(workflow, "admit_bundle", lambda _path: admitted)
    monkeypatch.setattr(
        phone_data, "device_data_spec", lambda *_args: SimpleNamespace(required_declarations=())
    )
    monkeypatch.setattr(phone_data, "prepare_phone_data", prepare)
    monkeypatch.setattr(boot, "reconnect_live_bundle", reconnect)
    monkeypatch.setattr(
        inputs, "capture_phone_data", lambda *_args, **_kwargs: pytest.fail("read card firmware")
    )
    monkeypatch.setattr(
        phone_data, "finish_reader", lambda *_args, **_kwargs: pytest.fail("finished reader")
    )

    status = card.run_card(ram, data, tmp_path / "logs", action="inspect", phone_data=store)

    output = capsys.readouterr()
    if reader_present:
        assert status == 0
        assert calls == ["prepare", "authenticate", "list"]
        assert f"SD card {CID} (4 MiB) at /dev/mmcblk0" in output.out
    else:
        assert status == 1
        assert calls == ["prepare", "authenticate"]
        assert "The phone left its RAM preparation session" in output.err


@pytest.mark.parametrize(
    ("listing", "erase_card", "expected_error"),
    [
        (f"/dev/mmcblk0 {CID} 8192\n", "a" * 32, "must match exactly one SD card"),
        (
            f"/dev/mmcblk0 {CID} 8192\n/dev/mmcblk1 {'a' * 32} 8192\n",
            None,
            "Select the card with --erase-card CID",
        ),
        ("", CID, "No microSD card was found"),
        (f"/dev/mmcblk0 {CID} 8192\n", None, "Card installation cancelled"),
        (f"/dev/mmcblk0 {CID} 4096\n", CID, "The selected SD card is too small"),
    ],
    ids=["mismatched-cid", "multiple-unselected", "no-card", "declined", "too-small"],
)
def test_card_install_rejects_before_sending_or_writing_to_the_card(
    bundle: Path,
    monkeypatch: pytest.MonkeyPatch,
    listing: str,
    erase_card: str | None,
    expected_error: str,
) -> None:
    session = {"session": "authenticated reader"}
    messages: list[str] = []
    log = SimpleNamespace(message=messages.append, capture=nullcontext)
    confirmations: list[str] = []
    remote_commands: list[str] = []

    def confirm(selected: card.SdCard) -> bool:
        assert "Shared files (FAT32 partition): 2 MiB" in messages
        ext4_capacity = (
            "FPLinux (ext4 partition): 1 MiB initially, 1 MiB after first-boot expansion"
        )
        assert ext4_capacity in messages
        confirmations.append(selected.cid)
        return False

    def remote(_session: object, command: str, **_kwargs: object) -> SimpleNamespace:
        remote_commands.append(command)
        assert "/sys/block/mmcblk" in command
        return SimpleNamespace(returncode=0, stdout=listing)

    def start_reader(*_args: Any, **options: Any) -> int:  # noqa: ANN401 -- workflow peer.
        options["install_card"]((SimpleNamespace(run_remote=remote), session), {}, log)
        return 0

    monkeypatch.setattr(card, "run_phone", start_reader)
    monkeypatch.setattr(
        card, "_write_card", lambda *_args, **_kwargs: pytest.fail("started writer")
    )

    with pytest.raises(SystemExit, match=expected_error):
        card.run_card(
            bundle, bundle / "data", bundle / "logs", erase_card=erase_card, confirm=confirm
        )

    assert confirmations == ([CID] if expected_error == "Card installation cancelled" else [])
    assert len(remote_commands) == 1


@pytest.mark.parametrize("outcome", ["invalid-host-key", "interrupted", "verified"])
def test_card_writer_streams_the_repository_and_preserves_the_bound_ssh_keys(
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
        nonlocal remote_cleaned
        if command.startswith("mktemp "):
            return SimpleNamespace(returncode=0, stdout=f"{remote_staging}\n")
        if command.startswith("/usr/libexec/fplinux/preparation "):
            progress_commands.append(command)
            return SimpleNamespace(returncode=0, stdout="")
        if command.startswith(f"sh {remote_staging}/"):
            arguments = shlex.split(command)
            assert arguments[2:] == ["prepare-access", remote_staging]
            return SimpleNamespace(
                returncode=0,
                stdout="unavailable\n" if outcome == "invalid-host-key" else HOST_KEY + "\n",
            )
        if command.startswith("rm -f "):
            assert {
                f"{remote_staging}/default-apps",
                f"{remote_staging}/apks.sha256",
                f"{remote_staging}/card-apk-world.sh",
            }.issubset(shlex.split(command))
            remote_cleaned = True
            return SimpleNamespace(returncode=0)
        if "mmcblk" in command:
            return SimpleNamespace(returncode=0, stdout=f"/dev/mmcblk0 {CID} 8192\n")
        pytest.fail(f"unexpected remote action: {command}")

    def send(
        _session: object, command: str, write: Callable[[IO[bytes]], None]
    ) -> SimpleNamespace:
        nonlocal credentials, writer_started
        writer_started = True
        arguments = shlex.split(command)
        assert arguments[:2] == ["sh", f"{remote_staging}/install-card.sh"]
        assert arguments[2:5] == ["/dev/mmcblk0", CID, remote_staging]
        assert arguments[5:15] == [
            "1",
            "2",
            "3",
            "1",
            "2",
            "a" * 64,
            "c" * 64,
            "b" * 64,
            TARGET,
            RELEASE,
        ]
        assert arguments[15:-1] == [
            hashlib.sha256(uploaded[f"{remote_staging}/{name}"]).hexdigest()
            for name in (
                "board.tar",
                "personal.tar",
                "installed.sha256",
                "fit.itb",
                "default-apps",
                "apks.sha256",
            )
        ]
        assert uploaded[f"{remote_staging}/card-slots.sh"]
        assert uploaded[f"{remote_staging}/card-apk-world.sh"]
        assert arguments[-1] == HOST_KEY
        device_id = uploaded[f"{remote_staging}/device-id"]
        assert len(device_id) == 32
        serial = hashlib.sha256(device_id).hexdigest()[:32]
        usb = uploaded[f"{remote_staging}/usb-network"]
        assert len(usb) == 256
        network_fields = dict(line.split("=") for line in usb.rstrip(b"\0").decode().splitlines())
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

        stream = BytesIO()
        write(stream)
        stream.seek(0)
        repository = {
            "Packages.adb": b"signed index",
            "fplinux-build.rsa.pub": b"release public key",
            "fplinux-notes.apk": b"notes package",
        }
        with tarfile.open(fileobj=stream, mode="r:") as archive:
            assert set(archive.getnames()) == repository.keys()
            for name, contents in repository.items():
                source = archive.extractfile(name)
                assert source is not None
                with source:
                    assert source.read() == contents
        checksums = uploaded[f"{remote_staging}/apks.sha256"].decode("ascii")
        assert set(checksums.splitlines()) == {
            f"{hashlib.sha256(contents).hexdigest()}  {name}"
            for name, contents in repository.items()
        }
        assert set(repository.values()).isdisjoint(uploaded.values())
        assert uploaded[f"{remote_staging}/default-apps"] == b"fplinux-notes\n"
        return SimpleNamespace(returncode=0, stderr="")

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
        send_remote=send,
        upload=upload,
    )

    def start_reader(*_args: Any, **options: Any) -> int:  # noqa: ANN401 -- workflow peer.
        options["install_card"]((transport, session), {}, log)
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
