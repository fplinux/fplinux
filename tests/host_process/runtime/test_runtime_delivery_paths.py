# SPDX-License-Identifier: GPL-2.0-only
"""Exercise checkout selection and extracted ZIPs through the same real runtime.

Local executables replace the USB loader, bridge, SSH endpoint and privileged
host commands. Session creation, authentication, backup, extraction, boot and
reconnect remain production code; this is not evidence about a physical phone.
"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
import sys
import zipfile
from dataclasses import dataclass, replace
from pathlib import Path
from typing import TYPE_CHECKING, Any

import pytest

from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.fixtures.portable_archive import (
    capsule_files,
    refresh_archive,
)
from tests.fixtures.portable_archive import (
    portable_archive as portable_archive,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

    from tests.fixtures.portable_archive import PortableArchive

PEERS = ROOT / "tests/host_process/runtime/fixtures/runtime-delivery-paths"


@dataclass(frozen=True)
class RuntimeDelivery:
    """Own one selected delivery, its private phone store and external process peers."""

    mode: str
    bundle: Path
    checkout: Path
    data: Path
    phone_store: Path
    logs: Path
    dump: Path
    home_environment: dict[str, str]

    @property
    def boundary(self) -> Path:
        """Return only the controlled external endpoint's state."""
        return self.bundle / "boundary"

    def run(self, action: str = "run") -> subprocess.CompletedProcess[str]:
        """Enter the real selector or delivered entry with an owned input terminal."""
        return run_process(
            [
                sys.executable,
                "-I",
                str(PEERS / "delivery_terminal.py"),
                self.mode,
                str(ROOT),
                str(self.checkout),
                str(self.bundle),
                action,
            ],
            name=f"{self.mode} runtime lifecycle",
            cwd=self.bundle.parent,
            env={
                **self.home_environment,
                "PATH": str(self.bundle / "bin"),
                "LANG": "C.UTF-8",
                "PYTHONDONTWRITEBYTECODE": "1",
                "FPLINUX_AUTONOMOUS_TEST_ROOT": str(self.bundle),
                "FPLINUX_RUNTIME_DELIVERY_DATA": str(self.data),
                "FPLINUX_RUNTIME_DELIVERY_NAND": str(self.dump),
            },
            timeout=60,
        )

    def shells(self) -> list[dict[str, Any]]:
        """Read sessions received by separate SSH shell processes."""
        return [
            json.loads(line) for line in (self.boundary / "shells.jsonl").read_text().splitlines()
        ]


def _install_external_boundaries(root: Path) -> None:
    """Keep the real boot path while replacing unavailable USB and host process edges."""
    boundary = root / "boundary"
    boundary.mkdir()
    (boundary / "bootrom").touch()
    shutil.copyfile(ROOT / "tests/fixtures/nand-geometry/autonomous.txt", boundary / "geometry")
    (root / "60-fplinux.rules").write_bytes(b'SUBSYSTEM=="usb", MODE="0660"\n')
    (root / "image/ramboot.bin").write_bytes(b"DHTB" + b"p" * 1020 + bytes(512) + b"suffix")
    discovery = (
        ROOT / "tests/host_process/runtime/fixtures/autonomous-runner/autonomous_usb_boundary.py"
    ).read_text(encoding="utf-8")
    for relative in (
        "platforms/ums9117/host/adapter.py",
        "scripts/fplinux_cli/runtime/ssh_transport.py",
    ):
        with (root / relative).open("a", encoding="utf-8") as stream:
            stream.write("\n" + discovery)
    with (root / "scripts/fplinux_cli/runtime/boot.py").open("a", encoding="utf-8") as stream:
        stream.write("\n" + (PEERS / "usb_rules_boundary.py").read_text(encoding="utf-8"))
    tools = root / "bin"
    tools.mkdir()
    for relative in (
        "tools/spd_dump.exe",
        "tools/libc_server.exe",
        "bin/ssh",
        "bin/ssh-keyscan",
        "bin/ip",
        "bin/sudo",
        "bin/udevadm",
    ):
        install_python_script(PEERS / "delivery_tool.py", root / relative)
    runtime_tools = ROOT / "tests/fixtures/runtime-tools"
    install_python_script(runtime_tools / "windows_keygen.py", tools / "ssh-keygen")
    shutil.copyfile(runtime_tools / "stdbuf.sh", tools / "stdbuf")
    (tools / "stdbuf").chmod(0o755)
    manifest_path = root / "runtime-manifest.json"
    runtime = json.loads(manifest_path.read_text(encoding="utf-8"))
    runtime["personalization"]["template_sha256"] = hashlib.sha256(bytes(512)).hexdigest()
    for device in runtime["usb"].values():
        device["wait_seconds"] = 3
    runtime["adapter"].update({"handoff_wait_seconds": 3, "usb_release_wait_seconds": 3})
    refresh_archive(root, runtime)


def _extract_delivery_zip(root: Path, *, recipient_name: str = "recipient") -> Path:
    """Transport the controlled runtime through an actual ZIP, preserving Unix file modes."""
    archive = root.parent / "runtime.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
        for path in sorted(root.rglob("*")):
            if path.is_file():
                output.write(path, Path("runtime") / path.relative_to(root))
    recipient = root.parent / recipient_name
    with zipfile.ZipFile(archive) as source:
        source.extractall(recipient)
        for member in source.infolist():
            (recipient / member.filename).chmod((member.external_attr >> 16) & 0o777)
    return recipient / "runtime"


@pytest.fixture(params=["checkout", "zip"])
def runtime_delivery(
    request: pytest.FixtureRequest, portable_archive: PortableArchive
) -> RuntimeDelivery:
    """Select one real bundle or extract its same delivery bytes without preparing phone data."""
    root = portable_archive.root
    phone_store = portable_archive.phone_store()
    _install_external_boundaries(root)
    checkout = root.parent / "checkout"
    checkout.mkdir()
    if request.param == "checkout":
        manifest = (root / "build-manifest.json").read_bytes()
        generation = json.loads(manifest)["generation"]
        slot = checkout / ".cache/out/nokia-ta1618/builds/release"
        bundles = slot / "bundles"
        bundles.mkdir(parents=True)
        bundle = bundles / generation
        shutil.move(root, bundle)
        (slot / "current.json").write_text(
            json.dumps(
                {"generation": generation, "manifest_sha256": hashlib.sha256(manifest).hexdigest()}
            ),
            encoding="utf-8",
        )
        data = checkout / ".cache/runtime/nokia-ta1618"
        logs = checkout / ".cache/logs/run/nokia-ta1618"
    else:
        bundle = _extract_delivery_zip(root)
        data = bundle / "data"
        logs = bundle / "logs"
    return RuntimeDelivery(
        mode=str(request.param),
        bundle=bundle,
        checkout=checkout,
        data=data,
        phone_store=phone_store,
        logs=logs,
        dump=portable_archive.dump,
        home_environment=portable_archive.environment(),
    )


def _file_sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def test_first_prepare_then_boot_and_reconnect_share_the_selected_runtime(
    runtime_delivery: RuntimeDelivery,
) -> None:
    """Both delivery paths preserve one backup and reuse the authenticated normal RAM session."""
    delivery = runtime_delivery

    first = delivery.run()

    assert first.returncode == 0, first.stderr
    assert "Nokia 105 4G (TA-1618)" in first.stdout
    assert "Connect the powered-off phone while holding *." in first.stdout
    assert "Secure phone connection is ready." in first.stdout
    assert "Phone clock set to 1970-01-01T00:20:34Z (RTC kept)." in first.stdout
    assert "Reading NAND:" in first.stdout
    assert "100.0%" in first.stdout
    assert "Run the same launcher again with the phone powered off." in first.stdout
    clock_path = delivery.boundary / "clock-commands"
    reader_clock_commands = clock_path.read_text().splitlines()
    assert len(reader_clock_commands) == 1
    assert re.fullmatch(r"fplinux-clock [1-9][0-9]*", reader_clock_commands[0])
    backup = delivery.phone_store / "nand.bin"
    backup_digest = _file_sha256(delivery.dump)
    assert backup.stat().st_size == 138412032
    assert _file_sha256(backup) == backup_digest
    backup_stat = backup.stat()
    receipt_path = delivery.phone_store / "nand.bin.json"
    receipt = receipt_path.read_bytes()
    assert json.loads(receipt) == {
        "id_bytes": "e521",
        "chip": "controlled chip",
        "page_main_bytes": 2048,
        "oob_bytes": 64,
        "pages_per_block": 64,
        "block_count": 1024,
        "raw_bytes": 138412032,
        "sha256": backup_digest,
        "target": "nokia-ta1618",
        "acquisition_recipe": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    }
    phone = delivery.phone_store / "phone"
    assert (phone / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (phone / "groups/audio-profile/audio.bin").read_bytes() == b"profile"
    assert (phone / "groups/fm-radio/fm.bin").read_bytes() == b"fm"
    assert capsule_files((delivery.boundary / "capsules/1.cpio").read_bytes()) == {
        "etc/fplinux/preparation": b"1\n"
    }
    progress_path = delivery.boundary / "preparation-commands"
    progress_commands = progress_path.read_text()
    updates = [json.loads(line) for line in progress_commands.splitlines()]
    assert updates[0] == [
        "/usr/libexec/fplinux/preparation",
        "--stage",
        "waiting",
        "/run/fplinux-preparation-status",
    ]
    assert [arguments[2] for arguments in updates[-2:]] == ["extracting", "ready"]
    reading = updates[1:-2]
    assert len(reading) >= 2
    assert all(
        arguments[:4] == ["/usr/libexec/fplinux/preparation", "--stage", "reading", "--completed"]
        for arguments in reading
    )
    assert all(
        arguments[5:] == ["--total", "138412032", "/run/fplinux-preparation-status"]
        for arguments in reading
    )
    completed = [int(arguments[4]) for arguments in reading]
    assert completed[0] == 0
    assert completed[-1] == 138412032
    assert completed == sorted(completed)
    assert "phone preparation progress is unavailable" not in first.stdout
    assert not (delivery.boundary / "capsules/2.cpio").exists()
    assert not (delivery.boundary / "shells.jsonl").exists()
    assert not (delivery.data / "reader").exists()
    assert not (delivery.data / "session").exists()
    assert (delivery.data / ".launcher.lock").is_file()
    reader_commands = (delivery.boundary / "reader-commands").read_text()
    assert reader_commands.splitlines() == [
        "cat /sys/class/misc/raw-nand/device/geometry 3</dev/raw-nand",
        "exec dd if=/dev/raw-nand bs=63360",
    ]
    (delivery.boundary / "linux-usb").rmdir()
    (delivery.boundary / "bootrom").touch()

    second = delivery.run()

    assert second.returncode == 0, second.stderr
    assert "Opening SSH." in second.stdout
    assert "SSH terminal closed." in second.stdout
    assert "Phone clock set to 1970-01-01T00:20:34Z (RTC kept)." in second.stdout
    assert "Reading NAND:" not in second.stdout
    assert "Preparing your phone data" not in second.stdout
    assert capsule_files((delivery.boundary / "capsules/2.cpio").read_bytes()) == {
        "lib/firmware/chip/radio.bin": b"radio",
        "lib/firmware/chip/fm.bin": b"fm",
        "lib/firmware/fplinux/audio.bin": b"profile",
    }
    session_path = delivery.data / "session/current/nokia-ta1618.json"
    session_bytes = session_path.read_bytes()
    session = json.loads(session_bytes)
    assert session["target"] == "nokia-ta1618"
    assert session["status"] == "ready"
    assert session["runtime_root"] == str(delivery.data / "session")
    assert Path(session["private_key"]).is_relative_to(delivery.data / "session/sessions")
    assert Path(session["private_key"]).is_file()
    assert not Path(session["image"]).exists()
    assert delivery.shells() == [session]
    clock_commands = clock_path.read_text()
    fresh_boot_clocks = clock_commands.splitlines()
    assert len(fresh_boot_clocks) == 2
    assert fresh_boot_clocks[:1] == reader_clock_commands
    assert re.fullmatch(r"fplinux-clock [1-9][0-9]*", fresh_boot_clocks[1])

    third = delivery.run()

    assert third.returncode == 0, third.stderr
    assert "Connecting to the running phone..." in third.stdout
    assert "Loading the phone into RAM" not in third.stdout
    assert "Opening SSH." in third.stdout
    assert "Phone clock" not in third.stdout
    assert session_path.read_bytes() == session_bytes
    assert delivery.shells() == [session, session]
    assert clock_path.read_text() == clock_commands
    assert (delivery.boundary / "ram-loads").read_text() == "loaded\nloaded\n"
    assert not (delivery.boundary / "capsules/3.cpio").exists()
    assert (delivery.boundary / "reader-commands").read_text() == reader_commands
    assert progress_path.read_text() == progress_commands
    journals = sorted(delivery.logs.glob("*/run.json"))
    assert len(journals) == 3
    for journal_path in journals:
        journal = json.loads(journal_path.read_text())
        assert journal["status"] == "success"
        assert journal["stages"] == [
            {"name": "runtime", "log": "runtime.log", "status": "success"}
        ]
        log = (journal_path.parent / "runtime.log").read_text()
        assert "controlled interactive shell output" not in log
    assert "controlled interactive shell output" in second.stdout
    assert "controlled interactive shell output" in third.stdout

    prepared_again = delivery.run("prepare")

    assert prepared_again.returncode == 0, prepared_again.stderr
    assert (phone / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (phone / "groups/audio-profile/audio.bin").read_bytes() == b"profile"
    assert (phone / "groups/fm-radio/fm.bin").read_bytes() == b"fm"
    assert _file_sha256(backup) == backup_digest
    final_stat = backup.stat()
    assert final_stat.st_ino == backup_stat.st_ino
    assert final_stat.st_mtime_ns == backup_stat.st_mtime_ns
    assert final_stat.st_ctime_ns == backup_stat.st_ctime_ns
    assert receipt_path.read_bytes() == receipt
    assert list(delivery.phone_store.rglob("nand.bin")) == [backup]
    assert not (delivery.data / "nand.bin").exists()
    assert not (delivery.data / "phone").exists()
    assert (delivery.boundary / "reader-commands").read_text() == reader_commands
    assert progress_path.read_text() == progress_commands
    assert (delivery.boundary / "ram-loads").read_text() == "loaded\nloaded\n"


def test_two_extracted_archives_share_raw_backup_and_reprepare_only_for_changed_recipe(
    portable_archive: PortableArchive,
) -> None:
    """Real ZIP entries share durable bytes while controlled processes replace the phone."""
    source = portable_archive.root
    _install_external_boundaries(source)
    first_bundle = _extract_delivery_zip(source, recipient_name="first-recipient")
    second_bundle = _extract_delivery_zip(source, recipient_name="second-recipient")
    first = RuntimeDelivery(
        mode="zip",
        checkout=source.parent / "unused-checkout",
        phone_store=portable_archive.phone_store(),
        dump=portable_archive.dump,
        home_environment=portable_archive.environment(),
        bundle=first_bundle,
        data=first_bundle / "data",
        logs=first_bundle / "logs",
    )
    second = replace(
        first,
        bundle=second_bundle,
        data=second_bundle / "data",
        logs=second_bundle / "logs",
    )

    acquired = first.run()

    assert acquired.returncode == 0, acquired.stderr
    assert "Reading NAND:" in acquired.stdout
    assert "Run the same launcher again" in acquired.stdout
    backup = first.phone_store / "nand.bin"
    before = backup.stat()
    receipt = (first.phone_store / "nand.bin.json").read_bytes()
    first_commands = (first.boundary / "reader-commands").read_text()
    assert first_commands.splitlines() == [
        "cat /sys/class/misc/raw-nand/device/geometry 3</dev/raw-nand",
        "exec dd if=/dev/raw-nand bs=63360",
    ]

    reused = second.run()

    assert reused.returncode == 0, reused.stderr
    assert "Opening SSH." in reused.stdout
    assert "Reading NAND:" not in reused.stdout
    assert not (second.boundary / "reader-commands").exists()
    assert capsule_files((second.boundary / "capsules/1.cpio").read_bytes()) == {
        "lib/firmware/chip/radio.bin": b"radio",
        "lib/firmware/chip/fm.bin": b"fm",
        "lib/firmware/fplinux/audio.bin": b"profile",
    }
    assert first.data != second.data
    assert not (first.data / "session").exists()
    assert (second.data / "session/current/nokia-ta1618.json").is_file()
    assert all(not (bundle / "data/nand.bin").exists() for bundle in (first_bundle, second_bundle))

    # A changed delivered parser selects a new base recipe, using the retained raw bytes.
    parser = second.bundle / "targets/nokia-ta1618/device_data.py"
    parser.write_bytes(parser.read_bytes().replace(b'b"profile"', b'b"updated"'))
    runtime_path = second.bundle / "runtime-manifest.json"
    runtime = json.loads(runtime_path.read_text(encoding="utf-8"))
    runtime["device_data"]["preparation_recipe"] = (
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
    )
    refresh_archive(second.bundle, runtime)
    (second.boundary / "linux-usb").rmdir()
    (second.boundary / "bootrom").touch()

    reprepared = second.run()

    assert reprepared.returncode == 0, reprepared.stderr
    assert "Opening SSH." in reprepared.stdout
    assert "Reading NAND:" not in reprepared.stdout
    assert not (second.boundary / "reader-commands").exists()
    assert (second.phone_store / "phone/groups/audio-profile/audio.bin").read_bytes() == b"updated"
    assert capsule_files((second.boundary / "capsules/2.cpio").read_bytes()) == {
        "lib/firmware/chip/radio.bin": b"radio",
        "lib/firmware/chip/fm.bin": b"fm",
        "lib/firmware/fplinux/audio.bin": b"updated",
    }
    after = backup.stat()
    assert (after.st_ino, after.st_mtime_ns, after.st_ctime_ns) == (
        before.st_ino,
        before.st_mtime_ns,
        before.st_ctime_ns,
    )
    assert (second.phone_store / "nand.bin.json").read_bytes() == receipt
    assert (first.boundary / "reader-commands").read_text() == first_commands

    # Acquisition changes require a fresh reader even when prepared outputs are available.
    runtime["nand"]["acquisition_recipe"] = (
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
    )
    refresh_archive(second.bundle, runtime)
    (second.boundary / "linux-usb").rmdir()
    (second.boundary / "bootrom").touch()

    reacquired = second.run()

    assert reacquired.returncode == 0, reacquired.stderr
    assert "Reading NAND:" in reacquired.stdout
    assert "Run the same launcher again" in reacquired.stdout
    assert (second.boundary / "reader-commands").read_text() == first_commands
    assert capsule_files((second.boundary / "capsules/3.cpio").read_bytes()) == {
        "etc/fplinux/preparation": b"1\n"
    }
    assert json.loads((second.phone_store / "nand.bin.json").read_text())[
        "acquisition_recipe"
    ] == ("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd")
