# SPDX-License-Identifier: GPL-2.0-only
"""Run isolated host processes using the runtime reconstructed from a release ZIP."""

from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from types import SimpleNamespace
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.build.runtime import device_data_descriptor
from fplinux_cli.runtime import bundle as bundle_runtime
from fplinux_cli.runtime.release_catalog import load_catalog, materialize_bundle

from tests import ROOT
from tests.host_process.packaging.autonomous_package_fixtures import (
    ArchivePhone,
    write_archive_file,
)
from tests.host_process.packaging.autonomous_package_fixtures import (
    archive_inputs as archive_inputs,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

    from tests.host_process.packaging.autonomous_package_fixtures import ArchiveInputs

_PEERS = ROOT / "tests/host_process/packaging/fixtures/autonomous-package"


def _materialize(inputs: ArchiveInputs, host: str) -> tuple[Path, Path]:
    archive_path = inputs.package(host)
    with zipfile.ZipFile(archive_path) as archive:
        archive.extractall(inputs.root / "recipient")  # noqa: S202 -- archive from controlled inputs.
    root = inputs.root / "recipient" / archive_path.stem
    catalog = load_catalog(root)
    assert set(catalog.targets) == {inputs.target}
    bundle = materialize_bundle(root, inputs.root / "recipient-data", catalog, inputs.target)
    return root, bundle


@pytest.fixture
def ecc_source_download(archive_inputs: ArchiveInputs) -> dict[str, bytes]:
    """Supply the pinned offline sources from which Linux packages its corrector."""
    revision = "0123456789abcdef0123456789abcdef01234567"
    contents = {
        "src/bch.c": b"synthetic BCH implementation\n",
        "src/bch.h": b"synthetic BCH header\n",
        "COPYING": b"synthetic GPL-2.0-only license\n",
    }
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz") as archive:
        for name, data in contents.items():
            item = tarfile.TarInfo(f"python-bchlib-{revision}/{name}")
            item.size = len(data)
            archive.addfile(item, io.BytesIO(data))
    data = stream.getvalue()
    write_archive_file(archive_inputs.root, ".data/downloads/sources/bch/bch.tar.gz", data)
    write_archive_file(
        archive_inputs.root,
        "sources.lock.toml",
        (archive_inputs.root / "sources.lock.toml").read_bytes()
        + (
            "\n"
            f'[bch]\nversion = "{revision}"\nname = "bch.tar.gz"\n'
            'url = "https://example.invalid/bch.tar.gz"\nalgorithm = "sha256"\n'
            f'checksum = "{hashlib.sha256(data).hexdigest()}"\n[bch.files]\n'
            f'bch_c_sha256 = "{hashlib.sha256(contents["src/bch.c"]).hexdigest()}"\n'
            f'bch_h_sha256 = "{hashlib.sha256(contents["src/bch.h"]).hexdigest()}"\n'
            f'copying_sha256 = "{hashlib.sha256(contents["COPYING"]).hexdigest()}"\n'
        ).encode(),
    )
    return contents


@pytest.mark.parametrize("host", ["linux", "windows"])
def test_release_launcher_and_reconstructed_runtime_work_without_the_checkout(
    archive_inputs: ArchiveInputs, host: str
) -> None:
    root, bundle = _materialize(archive_inputs, host)
    with mock.patch.object(
        bundle_runtime,
        "os",
        SimpleNamespace(
            name="nt" if host == "windows" else "posix", access=os.access, X_OK=os.X_OK
        ),
    ):
        admitted = bundle_runtime.admit_bundle(bundle)
    assert admitted.runtime["target"] == archive_inputs.target
    assert admitted.runtime["host_tools"] == (
        {"loader": "tools/spd_dump.exe", "bridge": "tools/libc_server.exe"}
        if host == "windows"
        else {"loader": "host/spd_dump", "bridge": "host/libc_server"}
    )
    if host == "windows":
        assert admitted.windows is not None
        assert (bundle / "licenses/windows-driver/License.rtf").read_bytes() == (
            b"synthetic driver redistribution license\n"
        )
        for name, contents in archive_inputs.windows_scripts.items():
            assert (bundle / name).read_bytes() == contents
    else:
        assert (bundle / "60-fplinux.rules").read_bytes() == (
            archive_inputs.bundle / "60-fplinux.rules"
        ).read_bytes()
        assert not (bundle / "common/windows").exists()

    help_result = run_process(
        [sys.executable, "-B", "-I", str(root / "fplinux.py"), "run", "--help"],
        name="release launcher help",
        cwd=root,
        env={},
        timeout=10,
    )
    assert help_result.returncode == 0, help_result.stderr
    assert "TARGET" in help_result.stdout
    assert "--target" not in help_result.stdout
    assert "--from-dump" in help_result.stdout
    assert "--loader-trace" in help_result.stdout
    assert "--loader-inputs" not in help_result.stdout
    assert "--ssh" not in help_result.stdout
    card_help = run_process(
        [sys.executable, "-B", "-I", str(root / "fplinux.py"), "verify", "--help"],
        name="release enrolled card help",
        cwd=root,
        env={},
        timeout=10,
    )
    assert card_help.returncode == 0, card_help.stderr
    assert "--card CID" in card_help.stdout

    console_help = run_process(
        [sys.executable, "-B", "-I", str(root / "fplinux.py"), "console", "--help"],
        name="release screenshot help",
        cwd=root,
        env={},
        timeout=10,
    )
    assert console_help.returncode == 0, console_help.stderr
    assert "--screenshot FILE" in console_help.stdout

    events = root.parent / "events.jsonl"
    result = run_process(
        [
            sys.executable,
            "-B",
            "-I",
            str(_PEERS / "autonomous_runtime_imports.py"),
            str(bundle),
            str(events),
        ],
        name="selected release runtime imports",
        cwd=root.parent,
        env={},
        timeout=10,
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == {"target": archive_inputs.target, "ecc": False}
    event = json.loads(events.read_bytes())
    event.pop("time")
    assert event == {
        "event": "waiting-for-device",
        "target": archive_inputs.target,
        "profile": None,
        "build_type": "release",
    }


@pytest.mark.parametrize("archive_inputs", ["xiaomi-qin-1s-plus"], indirect=True)
@pytest.mark.parametrize("host", ["linux", "windows"])
def test_qin_runtime_admits_the_selected_emmc_reader_and_native_tools(
    archive_inputs: ArchiveInputs, host: str
) -> None:
    _root, bundle = _materialize(archive_inputs, host)
    with mock.patch.object(
        bundle_runtime,
        "os",
        SimpleNamespace(
            name="nt" if host == "windows" else "posix", access=os.access, X_OK=os.X_OK
        ),
    ):
        admitted = bundle_runtime.admit_bundle(bundle)
    runtime = admitted.runtime
    assert runtime["nand"] is None
    assert runtime["emmc"]["raw_device"] == "/dev/mmcblk1"
    assert runtime["device_data"]["required_groups"] == ["audio-profile"]
    assert runtime["device_data"]["originals_parser"] == (
        "targets/xiaomi-qin-1s-plus/qin1splus_device_data.py"
    )
    assert (bundle / "targets/xiaomi-qin-1s-plus/device-data/rf-fields.tsv").is_file()
    for name in ("ext4", "cachetools", "crcmod"):
        assert f"scripts/{name}/__init__.py" in runtime["sha256"]
        assert (bundle / f"licenses/phone-data/{name}-LICENSE.txt").is_file()
    if host == "windows":
        assert runtime["host_tools"] == {"loader": "tools/spd_dump.exe"}
        windows = json.loads((bundle / "windows-host.json").read_bytes())
        assert windows["host_tools"] == {
            "loader": "tools/spd_dump.exe",
            "ssh": "tools/openssh/ssh.exe",
            "keygen": "tools/openssh/ssh-keygen.exe",
            "keyscan": "tools/openssh/ssh-keyscan.exe",
            "driver": "tools/driver/wdi-simple.exe",
        }
        assert not (bundle / "tools/libc_server.exe").exists()
    else:
        assert runtime["host_tools"] == {"loader": "host/spd_dump"}
        assert not (bundle / "host/libc_server").exists()
    assert not list(bundle.rglob("*nand-ecc*"))


@pytest.mark.parametrize("archive_inputs", ["bq-2830l-tank-connect-max"], indirect=True)
@pytest.mark.parametrize("host", ["linux", "windows"])
def test_release_delivers_a_loadable_nand_corrector_with_license_and_source(
    archive_inputs: ArchiveInputs, ecc_source_download: dict[str, bytes], host: str
) -> None:
    inputs = archive_inputs
    root, bundle = _materialize(inputs, host)
    tool = "tools/ums9117-nand-ecc.exe" if host == "windows" else "host/ums9117-nand-ecc"
    assert (bundle / tool).read_bytes() == b"prepared BCH decoder"
    assert (bundle / tool).stat().st_mode & 0o777 == 0o755
    companion = inputs.output / "FPLinux-1234567890ab-sources.zip"
    prefix = f"{companion.stem}/selections/{inputs.target}/{host}/sources"
    with zipfile.ZipFile(companion) as source:
        if host == "windows":
            assert source.read(f"{prefix}/windows-tools-source.zip") == (
                b"prepared compiler source projection"
            )
            assert (
                bundle / "licenses/windows-host/ums9117-nand-ecc-COPYING.txt"
            ).read_bytes() == (b"prepared BCH license")
        else:
            assert (bundle / "licenses/host-tools/ums9117-nand-ecc-COPYING.txt").read_bytes() == (
                ecc_source_download["COPYING"]
            )
            raw = source.read(f"{prefix}/ums9117-nand-ecc-source.zip")
            with zipfile.ZipFile(io.BytesIO(raw)) as archive:
                assert archive.read("ums9117-nand-ecc/bch.c") == ecc_source_download["src/bch.c"]
                assert archive.read("ums9117-nand-ecc/COPYING") == ecc_source_download["COPYING"]
                assert (
                    archive.read("ums9117-nand-ecc/Makefile")
                    == (inputs.root / "platforms/ums9117/host/nand-ecc/Makefile").read_bytes()
                )
                assert (
                    archive.read("ums9117-nand-ecc/fplinux-cli.h")
                    == (inputs.root / "include/fplinux/fplinux-cli.h").read_bytes()
                )

    result = run_process(
        [
            sys.executable,
            "-B",
            "-I",
            str(_PEERS / "autonomous_runtime_imports.py"),
            str(bundle),
            str(root.parent / "events.jsonl"),
        ],
        name="selected NAND corrector imports",
        cwd=root.parent,
        env={},
        timeout=10,
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == {"target": inputs.target, "ecc": True}


@pytest.mark.parametrize(
    "archive_inputs", [ArchivePhone("nokia-ta1618", required_phone_data=False)], indirect=True
)
@pytest.mark.parametrize("host", ["linux", "windows"])
def test_headless_archive_creates_a_transport_capsule_without_prepared_data(
    archive_inputs: ArchiveInputs, tmp_path: Path, host: str
) -> None:
    cpio = shutil.which("cpio")
    assert cpio is not None
    root, bundle = _materialize(archive_inputs, host)
    capsule = tmp_path / "transport.cpio"
    result = run_process(
        [
            sys.executable,
            "-B",
            "-I",
            str(_PEERS / "headless_runtime_imports.py"),
            str(bundle),
            str(capsule),
            host,
        ],
        name="headless release runtime",
        cwd=root.parent,
        env={},
        timeout=10,
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == {"target": archive_inputs.target, "preparation": None}
    destination = tmp_path / "capsule"
    destination.mkdir()
    extracted = subprocess.run(
        [
            cpio,
            "--extract",
            "--make-directories",
            "--no-absolute-filenames",
            "--no-preserve-owner",
        ],
        input=capsule.read_bytes(),
        cwd=destination,
        env={},
        timeout=10,
        capture_output=True,
        check=False,
    )
    assert extracted.returncode == 0, extracted.stderr
    assert {
        path.relative_to(destination).as_posix(): path.read_bytes()
        for path in destination.rglob("*")
        if path.is_file()
    } == {"etc/fplinux/usb-transport": b"rndis\n"}


def test_runtime_device_data_descriptor_records_the_selected_audio_parameters() -> None:
    config = {
        "platform": "ums9117",
        "linux": {"root": {"kind": "initramfs"}},
        "identity": {"compatible": "example,phone"},
        "audio_policy": {
            "speaker_vibration": False,
            "speaker_from_headphones": True,
            "headfree_headphones_only": False,
        },
        "nand": {"raw_device": "/dev/raw-nand", "id": 0xB1A1, "raw_page_bytes": 2176},
        "device_data": {
            "parser": "example_device_data.py",
            "groups": {
                "audio-profile": [
                    {"source": "audio.bin", "destination": "fplinux/audio.bin", "size": 788}
                ]
            },
        },
    }
    descriptor = device_data_descriptor("example-phone", config)
    assert descriptor is not None
    assert set(descriptor) == {
        "parser",
        "originals_parser",
        "groups",
        "required_groups",
        "audio_policy",
        "ecc_module",
    }
    assert descriptor["originals_parser"] is None
    assert descriptor["required_groups"] == ["audio-profile"]
    assert descriptor["parser"] == "targets/example-phone/example_device_data.py"
    assert descriptor["audio_policy"] == {
        "speaker_vibration": False,
        "speaker_from_headphones": True,
        "headfree_headphones_only": False,
    }


@pytest.mark.parametrize(
    ("platform", "root_kind", "groups"),
    [
        ("sc9820e", "initramfs", {"wcn-firmware": []}),
        ("ums9117", "external", {"audio-profile": []}),
        ("ums9117", "initramfs", {}),
    ],
)
def test_runtime_without_required_ram_data_has_no_preparation_descriptor(
    platform: str, root_kind: str, groups: dict[str, list[object]]
) -> None:
    assert (
        device_data_descriptor(
            "demo",
            {
                "platform": platform,
                "linux": {"root": {"kind": root_kind}},
                "device_data": {"parser": None, "groups": groups},
            },
        )
        is None
    )
