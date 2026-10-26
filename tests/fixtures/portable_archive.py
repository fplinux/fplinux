# SPDX-License-Identifier: GPL-2.0-only
"""Own isolated runtime deliveries with controlled NAND and target-parser inputs."""

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
from fplinux_cli.build.runtime import runtime_source_files

from tests import ROOT
from tests.bundle_support import file_record
from tests.windows_host_support import archive_files

if TYPE_CHECKING:
    from collections.abc import Iterator


def capsule_files(contents: bytes) -> dict[str, bytes]:
    """Read the public newc records with a small test-owned format reader."""
    result: dict[str, bytes] = {}
    offset = 0
    while True:
        header = contents[offset : offset + 110]
        assert header[:6] == b"070701"
        size = int(header[54:62], 16)
        namesize = int(header[94:102], 16)
        name = contents[offset + 110 : offset + 110 + namesize - 1].decode()
        offset = (offset + 110 + namesize + 3) & ~3
        if name == "TRAILER!!!":
            return result
        if int(header[14:22], 16) & 0o170000 == 0o100000:
            result[name] = contents[offset : offset + size]
        offset = (offset + size + 3) & ~3


@dataclass(frozen=True)
class PortableArchive:
    """Own a delivered runtime and its independently retained NAND input."""

    root: Path
    dump: Path
    manifest: dict[str, Any]
    data_home: Path

    def environment(self) -> dict[str, str]:
        """Isolate both durable phone data and host configuration from the real account."""
        environment = dict(os.environ)
        environment.pop("PYTHONPATH", None)
        environment.update(
            {
                "HOME": str(self.data_home.parent / "home"),
                "XDG_DATA_HOME": str(self.data_home),
                "LOCALAPPDATA": str(self.data_home.parent / "local-app-data"),
                "PYTHONDONTWRITEBYTECODE": "1",
            }
        )
        return environment

    def run(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run the same fixed-bundle entry without checkout imports."""
        return self.run_entrypoint("fplinux.py", "run", *arguments)

    def run_entrypoint(self, filename: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run delivered bytes with isolated imports and no ambient Python path."""
        return subprocess.run(
            [sys.executable, "-I", str(self.root / filename), *arguments],
            cwd=self.root.parent,
            env=self.environment(),
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )

    def prepared_data(self) -> Path:
        """Return the selected flat phone store."""
        return self.phone_store() / "phone"

    def phone_store(self) -> Path:
        """Return durable target data independently of the extracted archive location."""
        runtime = json.loads((self.root / "runtime-manifest.json").read_text(encoding="utf-8"))
        return self.data_home / "fplinux/phones" / str(runtime["target"])


def refresh_archive(root: Path, runtime: dict[str, Any] | None = None) -> None:
    """Publish fixture content receipts after replacing an external boundary."""
    runtime_path = root / "runtime-manifest.json"
    if runtime is None:
        runtime = json.loads(runtime_path.read_text(encoding="utf-8"))
    for relative in runtime["sha256"]:
        runtime["sha256"][relative] = hashlib.sha256((root / relative).read_bytes()).hexdigest()
    runtime_path.write_text(json.dumps(runtime), encoding="utf-8")
    build_path = root / "build-manifest.json"
    build = json.loads(build_path.read_text(encoding="utf-8"))
    build.pop("generation")
    build["target"] = runtime["target"]
    for relative in ("runtime-manifest.json", runtime["image"]):
        build["files"][relative] = file_record(root / relative)
    payload = (json.dumps(build, sort_keys=True, separators=(",", ":")) + "\n").encode()
    build["generation"] = hashlib.sha256(payload).hexdigest()
    build_path.write_text(json.dumps(build), encoding="utf-8")


@pytest.fixture
def portable_archive(tmp_path: Path) -> Iterator[PortableArchive]:
    """Ship the actual runtime closure and a parser with literal firmware outputs."""
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    parent = Path(log_root) if log_root else tmp_path
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=parent, prefix="portable-archive-") as temporary:
        workspace = Path(temporary)
        archive = workspace / "archive"
        archive.mkdir()
        archive_files(archive)
        (archive / "windows-host.json").unlink()
        descriptor: dict[str, Any] = {
            "parser": "targets/nokia-ta1618/device_data.py",
            "originals_parser": None,
            "required_groups": ["bluetooth", "audio-profile", "fm-radio"],
            "preparation_recipe": "b" * 64,
            "audio_policy": {
                "speaker_vibration": False,
                "speaker_from_headphones": False,
                "headfree_headphones_only": False,
            },
            "groups": {
                "bluetooth": [{"source": "radio.bin", "destination": "chip/radio.bin", "size": 5}],
                "audio-profile": [
                    {"source": "audio.bin", "destination": "fplinux/audio.bin", "size": 7}
                ],
                "fm-radio": [{"source": "fm.bin", "destination": "chip/fm.bin", "size": 2}],
            },
            "ecc_module": None,
        }
        nand = {
            "id": 0x21E5,
            "raw_page_bytes": 2112,
            "raw_device": "/dev/raw-nand",
            "acquisition_recipe": "a" * 64,
        }
        config = {
            "platform": "ums9117",
            "linux": {"root": {"kind": "initramfs"}},
            "device_data": {"groups": descriptor["groups"], "parser": "device_data.py"},
            "audio_policy": descriptor["audio_policy"],
            "nand": nand,
        }
        runtime_path = archive / "runtime-manifest.json"
        runtime = json.loads(runtime_path.read_text(encoding="utf-8"))
        sources = runtime_source_files("nokia-ta1618", config)
        sources[descriptor["parser"]] = (
            ROOT / "tests/fixtures/portable-archive/portable_device_data_parser.py"
        )
        for relative, source in sources.items():
            delivered = archive / relative
            delivered.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, delivered)
            runtime["sha256"][relative] = ""
        for relative in runtime["host_tools"].values():
            (archive / relative).chmod(0o755)
        runtime["target"] = "nokia-ta1618"
        runtime["identity"]["target"] = {
            "brand": "Nokia",
            "product": "105 4G",
            "hardware_codes": ["TA-1618"],
            "compatible": "nokia,ta1618",
            "display_name": "Nokia 105 4G (TA-1618)",
        }
        runtime["adapter"]["boot_instructions"] = "Connect the powered-off phone while holding *."
        runtime["device_data"] = descriptor
        runtime["nand"] = nand
        refresh_archive(archive, runtime)
        dump = workspace / "saved-nand.bin"
        with dump.open("wb") as stream:
            stream.truncate(138412032)
            stream.write(b"radio")
        with dump.open("rb") as stream:
            dump_digest = hashlib.file_digest(stream, "sha256").hexdigest()
        dump.with_name(f"{dump.name}.json").write_text(
            json.dumps(
                {
                    "id_bytes": "e521",
                    "chip": "controlled chip",
                    "page_main_bytes": 2048,
                    "oob_bytes": 64,
                    "pages_per_block": 64,
                    "block_count": 1024,
                    "raw_bytes": 138412032,
                    "sha256": dump_digest,
                    "target": "nokia-ta1618",
                    "acquisition_recipe": (
                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                    ),
                }
            ),
            encoding="utf-8",
        )
        data_home = workspace / "user-data"
        for directory in (data_home, workspace / "home", workspace / "local-app-data"):
            directory.mkdir()
        yield PortableArchive(archive, dump, descriptor, data_home)
