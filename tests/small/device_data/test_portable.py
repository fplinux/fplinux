# SPDX-License-Identifier: GPL-2.0-only
"""Exercise standalone dump extraction with controlled parser and host-tool boundaries.

The archive imports, complete-dump admission, board-map extraction, group
validation and generation publication are real. A small target parser replaces
the fitted phone records, and an external fake replaces the compiled firmware
decoder. Unused dump pages are sparse; selected stock-image and partition-table
extents contain controlled bytes. ECC cases run the delivered Python projection
through an exact-response child replacing native BCH correction. These cases do
not establish fitted-data extraction, BCH correctness or native Windows execution.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.cli.autonomous_package import runtime_python_files

from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.fixtures.stock_board_report import STOCK_IMAGE, agreeing_vbm, backup, signed_image

if TYPE_CHECKING:
    from collections.abc import Iterator


@dataclass(frozen=True)
class PortableArchive:
    """Owned archive and immutable-input location for one host-process case."""

    root: Path
    dump: Path
    manifest: dict[str, Any]

    def run(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run the delivered entry point with no checkout Python path."""
        return self.run_entrypoint("prepare.py", *arguments)

    def run_entrypoint(self, filename: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run a delivered archive entry point with no checkout Python path."""
        environment = dict(os.environ)
        environment.pop("PYTHONPATH", None)
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        return subprocess.run(
            [sys.executable, "-I", str(self.root / filename), *arguments],
            cwd=self.root.parent,
            env=environment,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )

    def current_generation(self) -> Path:
        """Resolve the artifact selected by the delivered preparation command."""
        target_root = self.root / ".cache/device-data/nokia-ta1618"
        selected = (target_root / "current").read_text(encoding="ascii").strip()
        return target_root / "generations" / selected


@pytest.fixture
def portable_archive(tmp_path: Path) -> Iterator[PortableArchive]:
    """Ship only runtime sources, a single-target fixture and a controlled tool process."""
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    parent = Path(log_root) if log_root else tmp_path
    parent.mkdir(parents=True, exist_ok=True)
    # Keep full generation copies off the runner's memory-backed /tmp.
    with tempfile.TemporaryDirectory(dir=parent, prefix="portable-archive-") as temporary:
        workspace = Path(temporary)
        archive = workspace / "archive"
        archive.mkdir()
        shutil.copyfile(ROOT / "common/prepare.py", archive / "prepare.py")
        config: dict[str, Any] = {
            "device_data": {"groups": {"bluetooth": [], "audio-profile": [], "fm-radio": []}}
        }
        for relative in runtime_python_files(config):
            delivered = archive / relative
            delivered.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, delivered)
        target_root = archive / "targets/nokia-ta1618"
        target_root.mkdir(parents=True)
        shutil.copyfile(
            ROOT / "tests/fixtures/processes/portable_device_data_parser.py",
            target_root / "device_data.py",
        )
        provider = archive / "platforms/ums9117/host/stock_image.py"
        provider.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "platforms/ums9117/host/stock_image.py", provider)
        tools = archive / "tools"
        tools.mkdir()
        executable = tools / "fphelper_t117"
        shutil.copyfile(ROOT / "tests/fixtures/fphelper/fake_fphelper.py", executable)
        executable.chmod(0o755)

        image = signed_image(bytes(0x5000))
        pinmap = bytes.fromhex("b0002a40 10000000 ffffffff ffffffff")
        keys = bytes.fromhex("2a00 3100")
        scenario = {
            "image_sha256": hashlib.sha256(image).hexdigest(),
            "commands": {
                "unpack": {
                    "stdout": "0x800: init_table, start = 0x900, end = 0x910\n"
                    "ps_addr: 0x80100000\n"
                    "0x4800: pinmap (end = 0x4810)\n"
                    "0x4900: keymap, bootkey = 0x2a (STAR)\n",
                    "files": {"pinmap.bin": pinmap.hex(), "keymap.bin": keys.hex()},
                }
            },
        }
        (tools / "scenario.json").write_text(json.dumps(scenario), encoding="utf-8")
        manifest = {
            "target": "nokia-ta1618",
            "platform": "ums9117",
            "nand": {"id": 0x21E5, "raw_page_bytes": 2112},
            "device_data": {
                "parser": "device_data.py",
                "groups": {
                    "bluetooth": [
                        {"source": "radio.bin", "destination": "chip/radio.bin", "size": 5}
                    ],
                    "audio-profile": [
                        {"source": "audio.bin", "destination": "fplinux/audio.bin", "size": 7}
                    ],
                    "fm-radio": [{"source": "fm.bin", "destination": "chip/fm.bin", "size": 2}],
                    "board-maps": [
                        {"source": "pinmap.bin", "destination": "pinmap.bin"},
                        {"source": "keymap.bin", "destination": "keymap.bin"},
                    ],
                },
            },
        }
        (archive / "device-data-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        dump = workspace / "saved-nand.bin"
        small = backup(image, vbm=agreeing_vbm())
        with dump.open("wb") as stream:
            stream.truncate(138412032)
            stream.write(small.raw)
            stream.seek(0)
            stream.write(b"radio")
        yield PortableArchive(archive, dump, manifest)


@pytest.fixture
def portable_ecc_archive(portable_archive: PortableArchive) -> PortableArchive:
    """Deliver the real ECC projection with one selected partition and controlled BCH child."""
    archive = portable_archive
    shutil.copyfile(
        ROOT / "tests/fixtures/processes/portable_ecc_parser.py",
        archive.root / "targets/nokia-ta1618/device_data.py",
    )
    shutil.copyfile(
        ROOT / "platforms/ums9117/host/nand_ecc.py",
        archive.root / "platforms/ums9117/host/nand_ecc.py",
    )
    archive.manifest["nand"] = {
        "id": 0x81C8,
        "raw_page_bytes": 2176,
        "ecc_format": "ums9117-bch12",
    }
    archive.manifest["device_data"]["groups"] = {
        "bluetooth": [{"source": "radio.bin", "destination": "chip/radio.bin", "size": 8}],
    }
    (archive.root / "device-data-manifest.json").write_text(
        json.dumps(archive.manifest), encoding="utf-8"
    )
    selected_sector = b"B" * 100 + b"damaged!" + b"B" * 916
    page = (
        b"!" * 1024
        + selected_sector
        + b"\xffabc"
        + b"1" * 21
        + b"ignored"
        + b"defg"
        + b"2" * 21
        + b"outside"
        + b"Z" * 64
    )
    with archive.dump.open("wb") as stream:
        stream.truncate(142606336)
        stream.write(page + b"\xff" * 2176)
    tools = archive.root / "tools"
    install_python_script(
        ROOT / "tests/fixtures/processes/nand_ecc_child.py", tools / "ums9117-nand-ecc"
    )
    (tools / "input.bin").write_bytes(selected_sector + b"2" * 21)
    (tools / "output.bin").write_bytes(b"b" * 100 + b"selected" + b"b" * 916)
    (tools / "exit-status.txt").write_text("0", encoding="ascii")
    (tools / "diagnostic.txt").write_text("", encoding="utf-8")
    return archive


@pytest.mark.parametrize("geometry_source", ["target", "receipt"])
def test_standalone_dump_publishes_all_groups_and_keeps_the_input(
    portable_archive: PortableArchive, geometry_source: str
) -> None:
    archive = portable_archive
    with archive.dump.open("rb") as stream:
        original_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
    if geometry_source == "receipt":
        archive.manifest["nand"] = None
        (archive.root / "device-data-manifest.json").write_text(
            json.dumps(archive.manifest), encoding="utf-8"
        )
        receipt = {
            "target": "nokia-ta1618",
            "id_bytes": "e521",
            "chip": "controlled chip",
            "page_main_bytes": 2048,
            "oob_bytes": 64,
            "pages_per_block": 64,
            "block_count": 1024,
            "raw_bytes": 138412032,
            "sha256": original_sha256,
        }
        archive.dump.with_name(archive.dump.name + ".json").write_text(
            json.dumps(receipt), encoding="utf-8"
        )

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 0, result.stderr
    generation = archive.current_generation()
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (generation / "groups/audio-profile/audio.bin").read_bytes() == b"profile"
    assert (generation / "groups/fm-radio/fm.bin").read_bytes() == b"fm"
    assert (generation / "groups/board-maps/pinmap.bin").read_bytes() == bytes.fromhex(
        "b0002a40 10000000 ffffffff ffffffff"
    )
    assert (generation / "groups/board-maps/keymap.bin").read_bytes() == b"\x2a\x00\x31\x00"
    report = json.loads((generation / "reports/board-maps/board-report.json").read_text())
    assert report["keypad"]["boot_key"] == {"code": "0x2a", "key": "FPLINUX_KEY_STAR"}
    receipt = json.loads((generation / "receipt.json").read_text())
    assert receipt["source"] == {
        "kind": "saved-dump",
        "size": 138412032,
        "sha256": original_sha256,
    }
    with archive.dump.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == original_sha256
    assert "Review " in result.stdout


def test_repeated_dump_preparation_selects_the_new_data(portable_archive: PortableArchive) -> None:
    archive = portable_archive
    first = archive.run("--from-dump", str(archive.dump))
    assert first.returncode == 0, first.stderr
    old_generation = archive.current_generation()
    with archive.dump.open("r+b") as stream:
        stream.write(b"other")

    second = archive.run("--from-dump", str(archive.dump))

    assert second.returncode == 0, second.stderr
    generation = archive.current_generation()
    assert generation != old_generation
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"other"
    assert (old_generation / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    with (generation / "source/nand.bin").open("rb") as stream:
        assert stream.read(5) == b"other"


def test_archive_cli_has_no_target_selector(portable_archive: PortableArchive) -> None:
    result = portable_archive.run(
        "--from-dump", str(portable_archive.dump), "--target", "unregistered-phone"
    )

    assert result.returncode == 2
    assert "unrecognized arguments: --target unregistered-phone" in result.stderr
    assert not (portable_archive.root / ".cache").exists()


def test_board_map_extractor_uses_windows_executable_name(
    portable_archive: PortableArchive, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Select the native basename while running the controlled decoder on the host."""
    tools = portable_archive.root / "tools"
    (tools / "fphelper_t117").rename(tools / "fphelper_t117.exe")
    monkeypatch.setattr(STOCK_IMAGE, "os", SimpleNamespace(name="nt"))
    nand = backup(signed_image(bytes(0x5000)), vbm=agreeing_vbm())

    prepared = STOCK_IMAGE.prepare_board_maps(nand, host_tools=tools)

    assert prepared.prepared["keymap.bin"] == b"\x2a\x00\x31\x00"


def test_incomplete_dump_is_rejected_before_publication(portable_archive: PortableArchive) -> None:
    portable_archive.dump.write_bytes(b"incomplete NAND backup")

    result = portable_archive.run("--from-dump", str(portable_archive.dump))

    assert result.returncode == 1
    assert "expected a complete 138412032-byte physical NAND backup" in result.stderr
    assert not (portable_archive.root / ".cache").exists()


def test_declared_ecc_publishes_corrected_partition_and_preserves_raw_backup(
    portable_ecc_archive: PortableArchive,
) -> None:
    archive = portable_ecc_archive
    with archive.dump.open("rb") as stream:
        original_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 0, result.stderr
    generation = archive.current_generation()
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"selected"
    assert (generation / "originals/bluetooth/radio-original.bin").read_bytes() == b"selected"
    receipt = json.loads((generation / "receipt.json").read_text())
    assert receipt["source"] == {
        "kind": "saved-dump",
        "size": 142606336,
        "sha256": original_sha256,
    }
    for path in (archive.dump, generation / "source/nand.bin"):
        with path.open("rb") as stream:
            assert hashlib.file_digest(stream, "sha256").hexdigest() == original_sha256
            stream.seek(1124)
            assert stream.read(8) == b"damaged!"


def test_uncorrectable_selected_sector_keeps_the_previous_generation(
    portable_ecc_archive: PortableArchive,
) -> None:
    archive = portable_ecc_archive
    first = archive.run("--from-dump", str(archive.dump))
    assert first.returncode == 0, first.stderr
    old_generation = archive.current_generation()
    tools = archive.root / "tools"
    (tools / "exit-status.txt").write_text("1", encoding="ascii")
    (tools / "diagnostic.txt").write_text("uncorrectable codeword\n", encoding="utf-8")

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 1
    assert "NAND ECC correction at main offset 0x400: uncorrectable codeword" in result.stderr
    assert archive.current_generation() == old_generation
    assert (old_generation / "groups/bluetooth/radio.bin").read_bytes() == b"selected"
    assert list(old_generation.parent.iterdir()) == [old_generation]


@pytest.mark.parametrize("marker", [2048, 4224], ids=["first-page", "second-page"])
def test_corrected_partition_with_a_raw_factory_marker_is_not_published(
    portable_ecc_archive: PortableArchive, marker: int
) -> None:
    archive = portable_ecc_archive
    with archive.dump.open("r+b") as stream:
        stream.seek(marker)
        stream.write(b"\x00")

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 1
    assert "selected NAND block 0 is marked bad" in result.stderr
    target_root = archive.root / ".cache/device-data/nokia-ta1618"
    assert not (target_root / "current").exists()
    assert not list((target_root / "generations").iterdir())
    with archive.dump.open("rb") as stream:
        stream.seek(marker)
        assert stream.read(1) == b"\x00"
