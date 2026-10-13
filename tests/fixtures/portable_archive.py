# SPDX-License-Identifier: GPL-2.0-only
"""Owned standalone archives with controlled device-data and decoder inputs."""

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
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.cli.autonomous_package import runtime_python_files

from tests import ROOT
from tests.bundle_support import file_record
from tests.fixtures.stock_board_report import agreeing_vbm, backup, signed_image
from tests.windows_host_support import archive_files

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
                    "stdout": (ROOT / "tests/fixtures/stock-board-report/maps-only.txt").read_text(
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
