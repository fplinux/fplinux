# SPDX-License-Identifier: GPL-2.0-only
"""Public named-original admission and flat phone publication with synthetic inputs."""

from __future__ import annotations

import hashlib
import json
import shutil
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.runtime.workflow import launcher_lock

from tests.cli_support import ROOT, prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from pathlib import Path

ORIGINALS = {
    "wcnmodem-firmware.bin": b"abc",
    "system-connectivity_configure.ini": b"a",
    "system-connectivity_calibration.ini": b"b",
    "wcn-connectivity_configure.ini": b"c",
    "wcn-connectivity_calibration.ini": b"d",
}


@dataclass(frozen=True)
class DeviceDataCli:
    """An isolated source checkout and its test-owned original source record."""

    root: Path
    manifest: Path

    def run(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run the real public entrypoint without a build runtime or phone."""
        return run_process(
            [str(self.root / "fplinux"), "device-data", "prepare", "example", *arguments],
            name="public device-data original import",
            cwd=self.root,
            timeout=10,
        )

    def phone(self) -> Path:
        """Locate the phone store consumed by builds and the runtime."""
        return self.root / ".cache/device-data/example/phone"


@pytest.fixture
def device_data_cli(tmp_path: Path) -> DeviceDataCli:
    """Own a minimal real SC9820E target with a synthetic named-original preparer."""
    prepare_cli_checkout(tmp_path)
    for destination, source in (
        ("platforms/sc9820e/platform.toml", "tests/fixtures/sc9820e-manifest/platform.toml"),
        ("targets/example/target.toml", "tests/fixtures/sc9820e-manifest/target.toml"),
        (
            "targets/example/originals_parser.py",
            "tests/fixtures/original-files/originals_parser.py",
        ),
        ("profiles/default/profile.toml", "profiles/default/profile.toml"),
    ):
        path = tmp_path / destination
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / source, path)
    target = tmp_path / "targets/example/target.toml"
    declaration = (ROOT / "tests/fixtures/original-files/device-data.toml").read_bytes()
    target.write_bytes(target.read_bytes() + b"\n" + declaration)
    (tmp_path / "platforms/sc9820e/linux/overlay").mkdir(parents=True)
    (tmp_path / "targets/example/linux/overlay").mkdir(parents=True)
    (tmp_path / "targets/example/loader").mkdir()
    originals = tmp_path / "originals"
    originals.mkdir()
    for name, contents in ORIGINALS.items():
        (originals / name).write_bytes(contents)
    manifest = tmp_path / "originals.json"
    shutil.copyfile(ROOT / "tests/fixtures/original-files/originals.json", manifest)
    return DeviceDataCli(tmp_path, manifest)


def test_public_original_import_retains_exact_source_originals_outputs_and_private_modes(
    device_data_cli: DeviceDataCli,
) -> None:
    """The real parser, target import, materializer and publisher admit one complete source."""
    expected_manifest = device_data_cli.manifest.read_bytes()
    result = device_data_cli.run("--from-originals", str(device_data_cli.manifest), "--offline")

    assert result.returncode == 0, result.stderr
    phone = device_data_cli.phone()
    assert not (phone / "source").exists()
    assert not (phone / "originals").exists()
    assert device_data_cli.manifest.read_bytes() == expected_manifest
    for name, expected in ORIGINALS.items():
        assert (device_data_cli.root / "originals" / name).read_bytes() == expected
    for name, expected in {
        "wcnmodem.bin": b"abc",
        "system-config.bin": b"CFG:a",
        "system-cali.bin": b"CAL:b",
        "wcn-config.bin": b"CFG:c",
        "wcn-cali.bin": b"CAL:d",
    }.items():
        assert (phone / "groups/wcn-firmware" / name).read_bytes() == expected
    receipt = json.loads((phone / "receipt.json").read_bytes())
    assert receipt["source"] == {
        "kind": "original-files",
        "size": len(expected_manifest),
        "sha256": hashlib.sha256(expected_manifest).hexdigest(),
    }
    assert {item["source"] for item in receipt["groups"][0]["originals"]} == set(ORIGINALS)
    for path in phone.rglob("*"):
        assert path.stat().st_mode & 0o777 == (0o700 if path.is_dir() else 0o600)
    assert phone.stat().st_mode & 0o777 == 0o700
    assert not (device_data_cli.root / ".cache/kern").exists()
    assert not (device_data_cli.root / ".cache/out").exists()
    assert "./fplinux build example" in result.stdout
    assert "./fplinux run example" in result.stdout


@pytest.mark.parametrize(
    ("case", "message"),
    [
        ("target", "must select target example"),
        ("duplicate", "duplicate names"),
        ("unsafe-name", "normalized relative path"),
        ("missing-record", "complete five original files"),
        ("size", "does not match its manifest size and SHA-256"),
        ("digest", "does not match its manifest size and SHA-256"),
    ],
)
def test_public_original_import_rejects_mismatched_or_incomplete_source_before_staging(
    device_data_cli: DeviceDataCli, case: str, message: str
) -> None:
    """Input identity and completeness are admitted before phone data can be written."""
    manifest = json.loads(device_data_cli.manifest.read_bytes())
    if case == "target":
        manifest["target"] = "other-phone"
    elif case == "duplicate":
        manifest["files"].append(manifest["files"][0])
    elif case == "unsafe-name":
        manifest["files"][0]["name"] = "../outside.bin"
    elif case == "missing-record":
        manifest["files"].pop()
    elif case == "size":
        manifest["files"][0]["size"] = 4
    else:
        manifest["files"][0]["sha256"] = "0" * 64
    device_data_cli.manifest.write_text(json.dumps(manifest), encoding="utf-8")

    result = device_data_cli.run("--from-originals", str(device_data_cli.manifest))

    assert result.returncode == 1, result.stderr
    assert message in result.stderr
    assert not device_data_cli.phone().exists()
    assert not (device_data_cli.root / ".cache/out").exists()


def test_public_original_import_materialization_failure_preserves_previous_phone_data(
    device_data_cli: DeviceDataCli,
) -> None:
    """An unsatisfied output declaration cannot replace an earlier complete preparation."""
    first = device_data_cli.run("--from-originals", str(device_data_cli.manifest))
    assert first.returncode == 0, first.stderr
    phone = device_data_cli.phone()
    previous_receipt = (phone / "receipt.json").read_bytes()
    target = device_data_cli.root / "targets/example/target.toml"
    target.write_text(
        target.read_text(encoding="utf-8").replace("size = 5", "size = 6", 1),
        encoding="utf-8",
    )

    result = device_data_cli.run("--from-originals", str(device_data_cli.manifest))

    assert result.returncode == 1, result.stderr
    assert "device-data group wcn-firmware" in result.stderr
    assert (phone / "receipt.json").read_bytes() == previous_receipt
    assert (phone / "groups/wcn-firmware/system-config.bin").read_bytes() == b"CFG:a"
    assert list(phone.parent.glob(".phone-*")) == []
    assert [path for path in phone.parent.iterdir() if path.is_dir()] == [phone]


@pytest.mark.parametrize("source", ["originals", "nand"])
def test_public_saved_source_prepare_rejects_an_active_runtime_instance(
    device_data_cli: DeviceDataCli, source: str
) -> None:
    """The real public command shares the standalone launcher's data-instance lock."""
    data = device_data_cli.phone().parent
    data.mkdir(parents=True)
    if source == "nand":
        target = device_data_cli.root / "targets/example/target.toml"
        target.write_text(
            target.read_text(encoding="utf-8").replace("originals_parser =", "parser ="),
            encoding="utf-8",
        )
        arguments = ("--from-dump", str(device_data_cli.root / "unread-nand.bin"))
    else:
        arguments = ("--from-originals", str(device_data_cli.manifest))
    original_manifest = device_data_cli.manifest.read_bytes()

    with launcher_lock(data):
        result = device_data_cli.run(*arguments)

    assert result.returncode == 1, result.stderr
    assert "already running" in result.stderr
    assert not device_data_cli.phone().exists()
    assert list(data.glob(".phone-*")) == []
    assert device_data_cli.manifest.read_bytes() == original_manifest


@pytest.mark.parametrize("source", ["manifest", "directory", "file"])
def test_public_original_import_rejects_linked_sources(
    device_data_cli: DeviceDataCli, source: str
) -> None:
    """An import cannot follow a linked manifest, original directory or original file."""
    if source == "manifest":
        path = device_data_cli.manifest
    elif source == "directory":
        path = device_data_cli.root / "originals"
    else:
        path = device_data_cli.root / "originals/wcnmodem-firmware.bin"
    saved = path.with_name(f"saved-{path.name}")
    path.rename(saved)
    path.symlink_to(saved, target_is_directory=source == "directory")

    result = device_data_cli.run("--from-originals", str(device_data_cli.manifest))

    assert result.returncode == 1, result.stderr
    assert "missing or invalid" in result.stderr
    assert not device_data_cli.phone().exists()


@pytest.mark.parametrize(
    ("arguments", "status", "message"),
    [
        ((), 1, "requires --from-originals MANIFEST.json"),
        (("--from-dump", "missing.bin"), 1, "requires --from-originals MANIFEST.json"),
        (("--events", "events.jsonl"), 1, "requires --from-originals MANIFEST.json"),
        (
            ("--from-originals", "missing.json", "--from-dump", "missing.bin"),
            2,
            "not allowed with argument",
        ),
        (
            ("--from-originals", "missing.json", "--events", "events.jsonl"),
            2,
            "not allowed with argument",
        ),
        (
            ("--from-dump", "missing.bin", "--events", "events.jsonl"),
            2,
            "not allowed with argument",
        ),
    ],
)
def test_public_original_target_rejects_unsupported_modes_before_source_or_loader_access(
    device_data_cli: DeviceDataCli, arguments: tuple[str, ...], status: int, message: str
) -> None:
    """Missing or contradictory saved sources cannot fall through to NAND acquisition."""
    result = device_data_cli.run(*arguments)

    assert result.returncode == status, result.stderr
    assert message in result.stderr
    assert not (device_data_cli.root / ".cache/device-data").exists()
    assert not (device_data_cli.root / ".cache/out").exists()
    assert not (device_data_cli.root / "events.jsonl").exists()


def test_public_nand_target_rejects_original_manifest_without_changing_its_source_contract(
    device_data_cli: DeviceDataCli,
) -> None:
    """Selecting a saved original set never changes the physical NAND parser interface."""
    target = device_data_cli.root / "targets/example/target.toml"
    target.write_text(
        target.read_text(encoding="utf-8").replace("originals_parser =", "parser ="),
        encoding="utf-8",
    )

    result = device_data_cli.run("--from-originals", str(device_data_cli.manifest))

    assert result.returncode == 1, result.stderr
    assert "requires physical NAND data and does not support --from-originals" in result.stderr
    assert not (device_data_cli.root / ".cache/device-data").exists()
