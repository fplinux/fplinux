# SPDX-License-Identifier: GPL-2.0-only
"""Public named-original admission and flat phone publication with synthetic inputs."""

from __future__ import annotations

import hashlib
import json
import shutil
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.device_data.cache import current_backup
from fplinux_cli.device_data.store import locked_phone_store

from tests.cli_support import ROOT, prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator

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
        return self.root / "data/fplinux/phones/example/phone"


@pytest.fixture
def device_data_cli(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> DeviceDataCli:
    """Own a minimal real SC9820E target with a synthetic named-original preparer."""
    prepare_cli_checkout(tmp_path)
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "data"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "local"))
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
    result = device_data_cli.run("--from-originals", str(device_data_cli.manifest))

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
    assert not (device_data_cli.root / ".data/kern").exists()
    assert not (device_data_cli.root / ".data/out").exists()
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
    assert not (device_data_cli.root / ".data/out").exists()


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
def test_public_saved_source_prepare_rejects_an_active_store_writer(
    device_data_cli: DeviceDataCli, source: str
) -> None:
    """A second process cannot replace phone data while another writer owns the store."""
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

    with locked_phone_store("example"):
        result = device_data_cli.run(*arguments)

    assert result.returncode == 1, result.stderr
    assert "data is being updated" in result.stderr
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
            "--events: not allowed with argument --from-dump",
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
    assert "Traceback" not in result.stderr
    assert not (device_data_cli.root / ".data/device-data").exists()
    assert not (device_data_cli.root / ".data/out").exists()
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
    assert not (device_data_cli.root / ".data/device-data").exists()


@pytest.fixture
def nand_import_root(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[Path]:
    """Release the full-size imported NAND files before the next public workflow."""
    with TemporaryDirectory(dir=tmp_path) as directory:
        root = Path(directory)
        prepare_cli_checkout(root)
        monkeypatch.setenv("HOME", str(root / "home"))
        monkeypatch.setenv("XDG_DATA_HOME", str(root / "data"))
        monkeypatch.setenv("LOCALAPPDATA", str(root / "local"))
        yield root


@pytest.mark.parametrize(
    "with_receipt", [True, False], ids=["current-receipt", "unknown-acquisition"]
)
def test_public_nand_import_retains_canonical_source_and_only_known_acquisition_identity(
    nand_import_root: Path, *, with_receipt: bool
) -> None:
    """The public saved-source command retains reusable acquisition evidence without hardware."""
    root = nand_import_root
    for relative in (
        "platforms/ums9117/platform.toml",
        "platforms/ums9117/loader/assets.lock.toml",
        "sources.lock.toml",
        "profiles/default/profile.toml",
    ):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / relative, path)
    target = root / "targets/example/target.toml"
    declaration = (ROOT / "tests/fixtures/profile-config/target.toml").read_text(encoding="utf-8")
    declaration = (declaration % {"target": "example"}).replace(
        'platform = "demo"', 'platform = "ums9117"'
    )
    target.write_text(
        declaration
        + '\n[nand]\nraw_device = "/dev/test-nand"\nid = 0x21e5\nraw_page_bytes = 2112\n',
        encoding="utf-8",
    )
    shutil.copyfile(
        ROOT / "tests/public_workflow/fixtures/device-data-import/parser.py",
        root / "targets/example/radio_parser.py",
    )
    (root / "platforms/ums9117/linux/overlay").mkdir(parents=True)
    (root / "targets/example/linux/overlay").mkdir(parents=True)
    source = root / "external-nand.bin"
    with source.open("wb") as stream:
        stream.write(b"test")
        stream.truncate(138412032)
    with source.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    receipt = source.with_name("external-nand.bin.json")
    if with_receipt:
        receipt.write_text(
            json.dumps(
                {
                    "target": "example",
                    "acquisition_recipe": "a" * 64,
                    "id_bytes": "e521",
                    "chip": "controlled chip",
                    "page_main_bytes": 2048,
                    "oob_bytes": 64,
                    "pages_per_block": 64,
                    "block_count": 1024,
                    "raw_bytes": 138412032,
                    "sha256": digest,
                }
            ),
            encoding="utf-8",
        )
    source_receipt = receipt.read_bytes() if with_receipt else None

    result = run_process(
        [
            str(root / "fplinux"),
            "device-data",
            "prepare",
            "example",
            "--from-dump",
            str(source),
        ],
        name="public physical NAND import",
        cwd=root,
        timeout=30,
    )

    assert result.returncode == 0, result.stderr
    store = root / "data/fplinux/phones/example"
    assert (store / "phone/groups/bluetooth/radio.bin").read_bytes() == b"test"
    with (store / "nand.bin").open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == digest
    with source.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == digest
    admitted = current_backup(
        store / "nand.bin",
        target="example",
        nand={"id": 0x21E5, "raw_page_bytes": 2112, "acquisition_recipe": "a" * 64},
    )
    assert (admitted is not None) is with_receipt
    if with_receipt:
        assert json.loads((store / "nand.bin.json").read_bytes())["acquisition_recipe"] == "a" * 64
        assert receipt.read_bytes() == source_receipt
    else:
        assert not (store / "nand.bin.json").exists()
        assert not receipt.exists()
    for path in store.rglob("*"):
        assert path.stat().st_mode & 0o777 == (0o700 if path.is_dir() else 0o600)
    assert store.stat().st_mode & 0o777 == 0o700
    assert not (root / ".data/out").exists()
