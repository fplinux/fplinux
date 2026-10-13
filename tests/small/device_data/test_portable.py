# SPDX-License-Identifier: GPL-2.0-only
"""Exercise standalone dump extraction with controlled parser and host-tool boundaries.

The archive imports, complete-dump admission, board-map extraction, group
validation and prepared-data publication are real. A small target parser replaces
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
from tests.bundle_support import file_record
from tests.fixtures.executables import install_python_script
from tests.fixtures.stock_board_report import STOCK_IMAGE, agreeing_vbm, backup, signed_image
from tests.small.device_data.audio_expected import INOI_VIBRATE_TONE_SECTION
from tests.small.device_data.audio_fixtures import inoi_audio_records
from tests.small.device_data.headfree_fixtures import headphone_only_headfree_records
from tests.small.device_data.nv_fixtures import nv1
from tests.small.device_data.test_headphone_fed_audio import headphone_fed_records
from tests.windows_host_support import archive_files

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator


@dataclass(frozen=True)
class PortableArchive:
    """Owned archive and immutable-input location for one host-process case."""

    root: Path
    dump: Path
    manifest: dict[str, Any]

    def run(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run the delivered entry point with no checkout Python path."""
        return self.run_entrypoint("Run.py", *arguments)

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

    def prepared_data(self) -> Path:
        """Resolve the artifact selected by the delivered preparation command."""
        return self.root / "data/phone"


def _deliver_launcher(root: Path) -> None:
    """Add the canonical archive admission boundary to saved-dump cases."""
    archive_files(root)
    (root / "windows-host.json").unlink()
    (root / "tools/spd_dump.exe").chmod(0o755)
    (root / "tools/libc_server.exe").chmod(0o755)
    shutil.copyfile(ROOT / "common/autonomous.py", root / "Run.py")
    runtime_path = root / "runtime-manifest.json"
    runtime = json.loads(runtime_path.read_text())
    runtime["target"] = "nokia-ta1618"
    runtime["identity"]["target"] = {
        "brand": "Nokia",
        "product": "105 4G",
        "hardware_codes": ["TA-1618"],
        "compatible": "nokia,ta1618",
        "display_name": "Nokia 105 4G (TA-1618)",
    }
    runtime["adapter"]["boot_instructions"] = "Connect the powered-off phone while holding *."
    runtime_path.write_text(json.dumps(runtime), encoding="utf-8")
    build_path = root / "build-manifest.json"
    build = json.loads(build_path.read_text())
    build.pop("generation")
    build["target"] = "nokia-ta1618"
    build["files"]["runtime-manifest.json"] = file_record(runtime_path)
    payload = (json.dumps(build, sort_keys=True, separators=(",", ":")) + "\n").encode()
    build["generation"] = hashlib.sha256(payload).hexdigest()
    build_path.write_text(json.dumps(build), encoding="utf-8")


@pytest.fixture
def portable_archive(tmp_path: Path) -> Iterator[PortableArchive]:
    """Ship only runtime sources, a single-target fixture and a controlled tool process."""
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    parent = Path(log_root) if log_root else tmp_path
    parent.mkdir(parents=True, exist_ok=True)
    # Keep complete NAND inputs off the runner's memory-backed /tmp.
    with tempfile.TemporaryDirectory(dir=parent, prefix="portable-archive-") as temporary:
        workspace = Path(temporary)
        archive = workspace / "archive"
        archive.mkdir()
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
                    "stdout": (ROOT / "tests/fixtures/stock_board_report/maps-only.txt").read_text(
                        encoding="utf-8"
                    ),
                    "files": {"pinmap.bin": pinmap.hex(), "keymap.bin": keys.hex()},
                }
            },
        }
        (tools / "scenario.json").write_text(json.dumps(scenario), encoding="utf-8")
        manifest = {
            "target": "nokia-ta1618",
            "platform": "ums9117",
            "machine_compatible": "nokia,ta1618",
            "audio_policy": {
                "speaker_vibration": False,
                "speaker_from_headphones": False,
                "headfree_headphones_only": False,
            },
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
        _deliver_launcher(archive)
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
    generation = archive.prepared_data()
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (generation / "groups/audio-profile/audio.bin").read_bytes() == b"profile"
    assert (generation / "groups/fm-radio/fm.bin").read_bytes() == b"fm"
    assert (generation / "groups/board-maps/pinmap.bin").read_bytes() == bytes.fromhex(
        "b0002a40 10000000 ffffffff ffffffff"
    )
    assert (generation / "groups/board-maps/keymap.bin").read_bytes() == b"\x2a\x00\x31\x00"
    report = json.loads((generation / "reports/board-maps/board-report.json").read_text())
    assert report["keypad"]["boot_key"] == {"code": "0x2a", "key": "FPLINUX_KEY_STAR"}
    assert not (generation / "source").exists()
    assert not (generation / "originals").exists()
    with archive.dump.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == original_sha256
    assert "Your phone data is ready in data/phone." in result.stdout
    assert not (archive.root / ".cache").exists()


def test_repeated_dump_preparation_preserves_the_selected_phone_data(
    portable_archive: PortableArchive,
) -> None:
    archive = portable_archive
    first = archive.run("--from-dump", str(archive.dump))
    assert first.returncode == 0, first.stderr
    generation = archive.prepared_data()
    with archive.dump.open("r+b") as stream:
        stream.write(b"other")

    second = archive.run("--from-dump", str(archive.dump))

    assert second.returncode == 1
    assert "prepared phone data already exists" in second.stderr
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert not list((archive.root / "data").glob(".phone-*"))


@pytest.mark.parametrize(
    ("policy_name", "record_factory", "size", "expected_gains", "expected_tone"),
    [
        (
            "speaker_vibration",
            inoi_audio_records,
            822,
            bytes.fromhex("07 6c 61 56 4b 41 37 2d 23 1b 1a00 39 35 31 2d 29 21 1d 19 17"),
            INOI_VIBRATE_TONE_SECTION,
        ),
        (
            "speaker_from_headphones",
            headphone_fed_records,
            788,
            bytes.fromhex("07 6f 66 5d 54 4b 42 39 30 27 0000 60 58 50 48 40 38 30 28 20"),
            b"",
        ),
        (
            "headfree_headphones_only",
            headphone_only_headfree_records,
            788,
            bytes.fromhex("04 5f 57 4f 47 3f 37 2f 27 1f 2b00 57 4f 47 3f 37 2f 27 1f 17"),
            b"",
        ),
    ],
)
def test_standalone_profile_uses_the_selected_identity_and_audio_policy(  # noqa: PLR0913
    *,
    portable_archive: PortableArchive,
    policy_name: str,
    record_factory: Callable[[], tuple[dict[int, bytes], dict[int, bytes]]],
    size: int,
    expected_gains: bytes,
    expected_tone: bytes,
) -> None:
    """The delivered parser consumes the manifest snapshot without a source target manifest."""
    archive = portable_archive
    shutil.copyfile(
        ROOT / "tests/fixtures/processes/portable_audio_data_parser.py",
        archive.root / "targets/nokia-ta1618/device_data.py",
    )
    downloaded, protected = record_factory()
    with archive.dump.open("r+b") as stream:
        for block, records in enumerate((downloaded, protected)):
            main = nv1(tuple(sorted(records.items()))).ljust(131072, b"\xff")
            raw = b"".join(
                main[offset : offset + 2048] + b"\xff" * 64 for offset in range(0, len(main), 2048)
            )
            stream.seek(block * 135168)
            stream.write(raw)
    archive.manifest["audio_policy"][policy_name] = True
    archive.manifest["device_data"]["groups"] = {
        "audio-profile": [
            {
                "source": "example-audio-profile.bin",
                "destination": "fplinux/example-audio-profile.bin",
                "size": size,
            }
        ]
    }
    (archive.root / "device-data-manifest.json").write_text(
        json.dumps(archive.manifest), encoding="utf-8"
    )

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 0, result.stderr
    profile_path = archive.prepared_data() / "groups/audio-profile/example-audio-profile.bin"
    profile = profile_path.read_bytes()
    assert profile[:32] == b"FPAUDIO\0nokia,ta1618" + b"\0" * 12
    assert profile[32:53] == expected_gains
    assert profile[788:] == expected_tone


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
    generation = archive.prepared_data()
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"selected"
    assert not (generation / "originals").exists()
    with archive.dump.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == original_sha256
        stream.seek(1124)
        assert stream.read(8) == b"damaged!"


def test_uncorrectable_selected_sector_does_not_publish_phone_data(
    portable_ecc_archive: PortableArchive,
) -> None:
    archive = portable_ecc_archive
    tools = archive.root / "tools"
    (tools / "exit-status.txt").write_text("1", encoding="ascii")
    (tools / "diagnostic.txt").write_text("uncorrectable codeword\n", encoding="utf-8")

    result = archive.run("--from-dump", str(archive.dump))

    assert result.returncode == 1
    assert "NAND ECC correction at main offset 0x400: uncorrectable codeword" in result.stderr
    assert not archive.prepared_data().exists()
    assert not list((archive.root / "data").glob(".phone-*"))


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
    assert not archive.prepared_data().exists()
    assert not list((archive.root / "data").glob(".phone-*"))
    with archive.dump.open("rb") as stream:
        stream.seek(marker)
        assert stream.read(1) == b"\x00"
