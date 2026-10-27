# SPDX-License-Identifier: GPL-2.0-only
"""Ready archive behavior with synthetic builds and prepared native-tool boundaries."""

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
from fplinux_cli.artifacts.release import ReleaseContext
from fplinux_cli.build.runtime import device_data_descriptor
from fplinux_cli.cli.parser import create_parser
from fplinux_cli.manifests import targets
from fplinux_cli.packaging import archive as package
from fplinux_cli.packaging import autonomous as autonomous_package
from fplinux_cli.runtime import bundle as bundle_runtime
from fplinux_cli.workspace import build_inputs
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests import ROOT
from tests.small.cli.autonomous_package_fixtures import ArchivePhone, write_archive_file
from tests.small.cli.autonomous_package_fixtures import (
    archive_inputs as archive_inputs,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from pathlib import Path

    from tests.small.cli.autonomous_package_fixtures import ArchiveInputs


@pytest.fixture
def ecc_source_download(archive_inputs: ArchiveInputs) -> dict[str, bytes]:
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
    write_archive_file(archive_inputs.root, ".cache/downloads/sources/bch/bch.tar.gz", data)
    write_archive_file(
        archive_inputs.root,
        "sources.lock.toml",
        (
            f'[bch]\nversion = "{revision}"\nname = "bch.tar.gz"\n'
            'url = "https://example.invalid/bch.tar.gz"\nalgorithm = "sha256"\n'
            f'checksum = "{hashlib.sha256(data).hexdigest()}"\n[bch.files]\n'
            f'bch_c_sha256 = "{hashlib.sha256(contents["src/bch.c"]).hexdigest()}"\n'
            f'bch_h_sha256 = "{hashlib.sha256(contents["src/bch.h"]).hexdigest()}"\n'
            f'copying_sha256 = "{hashlib.sha256(contents["COPYING"]).hexdigest()}"\n'
        ).encode(),
    )
    return contents


def _archive(root: Path) -> tuple[Path, dict[str, bytes], dict[str, int]]:
    path = next(
        path
        for path in (root / ".cache/out/releases").glob("*.zip")
        if not path.name.endswith("-sources.zip")
    )
    with zipfile.ZipFile(path) as archive:
        prefix = f"{path.stem}/"
        files = {
            item.filename.removeprefix(prefix): archive.read(item) for item in archive.infolist()
        }
        modes = {
            item.filename.removeprefix(prefix): item.external_attr >> 16
            for item in archive.infolist()
        }
    return path, files, modes


@pytest.mark.parametrize("host", ["linux", "windows"])
def test_archive_readme_preserves_the_complete_host_instructions(host: str) -> None:
    config = {
        "identity": {"brand": "ACME", "product": "Demo", "hardware_codes": ["D-1"]},
        "adapter": {"boot_instructions": "Hold the left soft key while connecting USB."},
        "linux": {"root": {"kind": "initramfs"}},
    }
    readme = autonomous_package._readme(  # noqa: SLF001 -- compare the delivered document.
        config, target="demo-phone", host=host, preparation=True
    )
    documents = ROOT / "tests/small/cli/fixtures/package-readme"
    assert readme == (documents / f"autonomous-initramfs-{host}.txt").read_bytes()


@pytest.mark.parametrize(
    "archive_inputs",
    [
        "nokia-ta1618",
        "inoi-240-modern-4g",
        "inoi-244-modern-4g",
        "maxvi-k15n-4g",
        "bq-2830l-tank-connect-max",
        "uniwa-v1000-4g",
    ],
    indirect=True,
)
@pytest.mark.parametrize("host", ["linux", "windows"])
@pytest.mark.usefixtures("ecc_source_download")
def test_ready_archive_preserves_manifests_modes_sources_and_omits_personal_data(
    archive_inputs: ArchiveInputs, tmp_path: Path, host: str
) -> None:
    inputs = archive_inputs
    package.package_target(inputs.target, host=host)
    path, files, modes = _archive(inputs.root)
    original = path.read_bytes()
    assert path.name == f"FPLinux-{inputs.target}-{host}-x64-1234567890ab.zip"
    companion = path.with_name(path.stem + "-sources.zip")
    with zipfile.ZipFile(companion) as archive:
        metadata = json.loads(archive.read(f"{companion.stem}/DISTRIBUTION.json"))
        assert (
            archive.read(f"{companion.stem}/project/source.c")
            == b"controlled corresponding source\n"
        )
    assert metadata == {
        "release_id": "1234567890ab",
        "source_commit": "1234567890ab" + "0" * 28,
        "source_snapshot": "a" * 64,
        "runtimes": [
            {
                "archive": path.name,
                "target": inputs.target,
                "host": host,
                "identity": "f" * 64,
                "sha256": hashlib.sha256(original).hexdigest(),
            }
        ],
    }
    assert f"  {companion.name}\n".encode() in files["README.txt"]
    assert (
        not {
            "device-data.cpio",
            "device-data-manifest.json",
            "pinmap.bin",
            "keymap.bin",
            "CANDIDATE-NOTICE.txt",
            "DEVELOPMENT-NOTICE.txt",
        }
        & files.keys()
    )
    assert not any(
        name.startswith(("data/", "logs/", ".cache/", "apks/", "docs/", "sources/"))
        for name in files
    )
    assert b"BUILDER PERSONAL DATA MUST STAY PRIVATE" not in b"".join(files.values())
    assert "host/fplinux-usb-keyboard" not in files
    build = json.loads(files["build-manifest.json"])
    runtime = json.loads(files["runtime-manifest.json"])
    assert set(runtime["host_tools"]) == (
        {"loader", "bridge", "nand_ecc"}
        if inputs.target == "bq-2830l-tank-connect-max"
        else {"loader", "bridge"}
    )
    assert build["release_id"] == "1234567890ab"
    assert set(build["files"]) == set(files) - {"build-manifest.json", "SHA256SUMS"}
    for name, record in build["files"].items():
        assert record == {
            "mode": modes[name] & 0o777,
            "size": len(files[name]),
            "sha256": hashlib.sha256(files[name]).hexdigest(),
        }
    for name, digest in runtime["sha256"].items():
        assert hashlib.sha256(files[name]).hexdigest() == digest
    checksums = dict(line.split("  ")[::-1] for line in files["SHA256SUMS"].decode().splitlines())
    assert set(checksums) == set(files) - {"SHA256SUMS"}
    assert all(
        hashlib.sha256(files[name]).hexdigest() == digest for name, digest in checksums.items()
    )
    if host == "windows":
        assert (
            f"%LOCALAPPDATA%\\FPLinux\\phones\\{inputs.target}\n".encode() in files["README.txt"]
        )
        assert "Run.cmd" in files
        assert "common/windows/driver-consent.ps1" in files
        assert "common/windows/driver-elevate.ps1" in files
        assert "Run.sh" not in files
        assert not any(name.startswith("host/") for name in files)
        windows = json.loads(files["windows-host.json"])
        assert windows["host_tools"]["driver"] == "tools/driver/wdi-simple.exe"
        assert (
            files["licenses/windows-driver/License.rtf"]
            == b"synthetic driver redistribution license\n"
        )
        for name in inputs.native_files:
            assert {**runtime["sha256"], **windows["sha256"]}[name] == hashlib.sha256(
                files[name]
            ).hexdigest()
        for name, contents in inputs.windows_scripts.items():
            assert files[name] == contents
    else:
        assert f"~/.local/share/fplinux/phones/{inputs.target}\n".encode() in files["README.txt"]
        assert "Run.sh" in files
        assert "Run.cmd" not in files
        assert modes["Run.sh"] == 0o100755
        assert not any(name.startswith("common/windows/") for name in files)
        assert files["60-fplinux.rules"] == (inputs.bundle / "60-fplinux.rules").read_bytes()
    extracted = tmp_path / "recipient"
    with zipfile.ZipFile(path) as archive:
        archive.extractall(extracted)  # noqa: S202 -- test-owned ZIP from controlled inputs.
    archive_root = extracted / path.stem
    if host == "windows":
        with mock.patch.object(
            bundle_runtime, "os", SimpleNamespace(name="nt", access=os.access, X_OK=os.X_OK)
        ):
            admitted = bundle_runtime.admit_bundle(archive_root)
        assert admitted.runtime["target"] == inputs.target
        assert admitted.windows is not None
    help_result = subprocess.run(
        [sys.executable, "-I", str(archive_root / "fplinux.py"), "run", "--help"],
        cwd=archive_root,
        env={},
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    assert help_result.returncode == 0, help_result.stderr
    assert "--from-dump" in help_result.stdout
    assert "--ssh" not in help_result.stdout
    result = subprocess.run(
        [
            sys.executable,
            "-I",
            str(
                ROOT / "tests/small/cli/fixtures/autonomous-package/autonomous_runtime_imports.py"
            ),
            str(archive_root),
        ],
        cwd=extracted,
        env={"PYTHONDONTWRITEBYTECODE": "1"},
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == {
        "target": inputs.target,
        "ecc": inputs.target == "bq-2830l-tank-connect-max",
    }
    package.package_target(inputs.target, host=host)
    assert path.read_bytes() == original


@pytest.mark.parametrize("archive_inputs", ["bq-2830l-tank-connect-max"], indirect=True)
@pytest.mark.parametrize("host", ["linux", "windows"])
def test_declared_ecc_archive_delivers_corrector_license_and_rebuild_source(
    archive_inputs: ArchiveInputs, ecc_source_download: dict[str, bytes], host: str
) -> None:
    inputs = archive_inputs
    package.package_target(inputs.target, host=host)
    path, files, modes = _archive(inputs.root)
    tool = "tools/ums9117-nand-ecc.exe" if host == "windows" else "host/ums9117-nand-ecc"
    assert files[tool] == b"prepared BCH decoder"
    assert modes[tool] == 0o100755
    companion = path.with_name(path.stem + "-sources.zip")
    with zipfile.ZipFile(companion) as source:
        if host == "windows":
            assert (
                source.read(f"{companion.stem}/sources/windows-tools-source.zip")
                == b"prepared compiler source projection"
            )
            assert (
                files["licenses/windows-host/ums9117-nand-ecc-COPYING.txt"]
                == b"prepared BCH license"
            )
        else:
            assert (
                files["licenses/host-tools/ums9117-nand-ecc-COPYING.txt"]
                == ecc_source_download["COPYING"]
            )
            raw = source.read(f"{companion.stem}/sources/ums9117-nand-ecc-source.zip")
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


def test_package_rejects_a_stale_common_source_context_before_archive_publication(
    archive_inputs: ArchiveInputs, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(
        package, "current_release_context", lambda: ReleaseContext("b" * 12, "b" * 40, "c" * 64)
    )
    with pytest.raises(SystemExit, match="release context is stale"):
        package.package_target(archive_inputs.target)
    assert not (archive_inputs.root / ".cache/out/releases").exists()


def test_changed_native_bytes_cannot_overwrite_an_existing_release_name(
    archive_inputs: ArchiveInputs,
) -> None:
    package.package_target(archive_inputs.target)
    path, _files, _modes = _archive(archive_inputs.root)
    original = path.read_bytes()
    archive_inputs.linux_files["runtime/bin/python3.14"].write_bytes(b"different dependency")
    with pytest.raises(SystemExit, match="name collision with different bytes"):
        package.package_target(archive_inputs.target)
    assert path.read_bytes() == original


@pytest.mark.parametrize("archive_inputs", ["xiaomi-qin-1s-plus"], indirect=True)
@pytest.mark.parametrize("host", ["linux", "windows"])
def test_sc9820e_archive_delivers_clean_loaders_and_the_selected_native_runtime(
    archive_inputs: ArchiveInputs, tmp_path: Path, host: str
) -> None:
    package.package_target(archive_inputs.target, host=host)
    path, files, modes = _archive(archive_inputs.root)
    runtime = json.loads(files["runtime-manifest.json"])
    assert runtime["device_data"]["required_groups"] == ["audio-profile"]
    assert runtime["device_data"]["originals_parser"] == (
        "targets/xiaomi-qin-1s-plus/qin1splus_device_data.py"
    )
    assert runtime["nand"] is None
    assert runtime["emmc"]["raw_device"] == "/dev/mmcblk1"
    assert files["targets/xiaomi-qin-1s-plus/device-data/rf-fields.tsv"]
    for name in ("ext4", "cachetools", "crcmod"):
        assert f"scripts/{name}/__init__.py" in runtime["sha256"]
        assert f"licenses/phone-data/{name}-LICENSE.txt" in files
    loader = "tools/spd_dump.exe" if host == "windows" else "host/spd_dump"
    assert runtime["host_tools"] == {"loader": loader}
    for name in ("assets/fdl1.bin", "assets/sml.bin", "assets/fdl2.bin"):
        assert files[name] == (archive_inputs.bundle / name).read_bytes()
    assert "device-data.cpio" not in files
    assert "tools/libc_server.exe" not in files
    assert "host/libc_server" not in files
    assert not any("nand-ecc" in name for name in files)
    if host == "windows":
        assert "Run.cmd" in files
        assert "Run.sh" not in files
        assert "runtime/python.exe" in files
        assert "runtime/bin/python3.14" not in files
        assert not any(name.startswith("host/") for name in files)
        assert b"Windows have not been verified with this phone" in files["README.txt"]
        windows = json.loads(files["windows-host.json"])
        assert windows["host_tools"] == {
            "loader": "tools/spd_dump.exe",
            "ssh": "tools/openssh/ssh.exe",
            "keygen": "tools/openssh/ssh-keygen.exe",
            "keyscan": "tools/openssh/ssh-keyscan.exe",
            "driver": "tools/driver/wdi-simple.exe",
        }
        for name, digest in windows["sha256"].items():
            assert hashlib.sha256(files[name]).hexdigest() == digest
    else:
        assert modes["Run.sh"] == 0o100755
        assert "runtime/bin/python3.14" in files
    assert runtime["release_id"] == "1234567890ab"
    for name, digest in runtime["sha256"].items():
        assert hashlib.sha256(files[name]).hexdigest() == digest
    companion = path.with_name(path.stem + "-sources.zip")
    with zipfile.ZipFile(companion) as source:
        metadata = json.loads(source.read(f"{companion.stem}/DISTRIBUTION.json"))
    assert metadata["release_id"] == "1234567890ab"
    assert metadata["runtimes"][0]["host"] == host
    assert metadata["runtimes"][0]["sha256"] == hashlib.sha256(path.read_bytes()).hexdigest()
    assert "backup" not in files["README.txt"].decode()
    readme = " ".join(files["README.txt"].decode().split())
    assert "reads only the selected sound settings" in readme
    assert "selected original files in originals/" in readme
    assert "--from-dump" in readme
    assert "phone-user-area.bin" in readme
    assert "nand.bin.json" not in readme
    assert "RAM boot does not access internal storage" not in readme
    extracted = tmp_path / "recipient"
    with zipfile.ZipFile(path) as source:
        source.extractall(extracted)  # noqa: S202 -- test-owned ZIP from controlled inputs.
    archive_root = extracted / path.stem
    if host == "windows":
        with mock.patch.object(
            bundle_runtime, "os", SimpleNamespace(name="nt", access=os.access, X_OK=os.X_OK)
        ):
            admitted = bundle_runtime.admit_bundle(archive_root)
        assert admitted.runtime["target"] == "xiaomi-qin-1s-plus"
        assert admitted.windows is not None
    result = subprocess.run(
        [sys.executable, "-I", str(archive_root / "fplinux.py"), "run", "--help"],
        cwd=extracted,
        env={},
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert "--loader-inputs" not in result.stdout


@pytest.mark.parametrize("archive_inputs", ["xiaomi-qin-1s-plus"], indirect=True)
def test_sc9820e_windows_archive_rejects_external_root_before_collecting_inputs(
    archive_inputs: ArchiveInputs,
) -> None:
    config = targets.load_target(archive_inputs.target)
    config["linux"]["root"]["kind"] = "external"
    with pytest.raises(SystemExit, match="external-root exports support Linux x86-64 only"):
        package.package_target(archive_inputs.target, host="windows")
    assert not (archive_inputs.root / ".cache/out/releases").exists()


@pytest.mark.parametrize("archive_inputs", ["xiaomi-qin-1s-plus"], indirect=True)
def test_sc9820e_archive_rejects_a_build_with_another_device_data_selection(
    archive_inputs: ArchiveInputs, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest = json.loads(archive_inputs.manifest)
    context = ReleaseContext(
        manifest["release_id"], manifest["source_commit"], manifest["source_snapshot"]
    )

    def snapshot(
        *_args: object, include_device_data: bool = True, **_kwargs: object
    ) -> WorkspaceSnapshot:
        return WorkspaceSnapshot(
            (), ("a" if include_device_data else "9") * 64, release_context=context
        )

    monkeypatch.setattr(build_inputs, "target_workspace_snapshot", snapshot)
    with pytest.raises(SystemExit, match="build output is stale"):
        package.package_target(archive_inputs.target)


@pytest.mark.parametrize(
    "archive_inputs", [ArchivePhone("nokia-ta1618", required_phone_data=False)], indirect=True
)
@pytest.mark.parametrize("host", ["linux", "windows"])
def test_headless_archive_imports_and_creates_an_empty_transport_capsule(
    archive_inputs: ArchiveInputs, tmp_path: Path, host: str
) -> None:
    cpio = shutil.which("cpio")
    assert cpio is not None
    package.package_target(archive_inputs.target, host=host)
    path, files, modes = _archive(archive_inputs.root)
    assert json.loads(files["runtime-manifest.json"])["device_data"] is None
    recipient = tmp_path / "recipient"
    recipient.mkdir()
    with zipfile.ZipFile(path) as source:
        source.extractall(recipient)  # noqa: S202 -- owned ZIP from controlled inputs.
    archive_root = recipient / path.stem
    for name, mode in modes.items():
        (archive_root / name).chmod(mode & 0o777)
    capsule = tmp_path / "transport.cpio"
    result = subprocess.run(
        [
            sys.executable,
            "-I",
            str(ROOT / "tests/small/cli/fixtures/autonomous-package/headless_runtime_imports.py"),
            str(archive_root),
            str(capsule),
            host,
        ],
        cwd=recipient,
        env={},
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
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
        capture_output=True,
        timeout=10,
        check=False,
    )
    assert extracted.returncode == 0, extracted.stderr.decode()
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
    "arguments",
    [
        pytest.param(["--autonomous"], id="autonomous"),
        pytest.param(["--candidate"], id="candidate"),
        pytest.param(["--offline"], id="offline"),
        pytest.param(["--build-id", "1234567890ab"], id="build-id"),
        pytest.param(["--build-type", "release"], id="build-type"),
    ],
)
def test_package_parser_rejects_removed_options(
    arguments: list[str], capsys: pytest.CaptureFixture[str]
) -> None:
    parser, _check = create_parser(("nokia-ta1618",))
    with pytest.raises(SystemExit) as rejected:
        parser.parse_args(["package", "nokia-ta1618", *arguments])
    assert rejected.value.code == 2
    assert f"unrecognized arguments: {' '.join(arguments)}" in capsys.readouterr().err


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
