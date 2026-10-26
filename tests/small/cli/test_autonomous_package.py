# SPDX-License-Identifier: GPL-2.0-only
"""Archive behavior with synthetic built files and prepared native-tool boundaries."""

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
from fplinux_cli.artifacts.bundles import (
    create_bundle_staging,
    publish_bundle_generation,
    publish_current_bundle,
)
from fplinux_cli.build.runtime import device_data_descriptor
from fplinux_cli.cli.parser import create_parser
from fplinux_cli.common import canonical_json_bytes
from fplinux_cli.manifests import targets
from fplinux_cli.packaging import archive as package
from fplinux_cli.packaging import autonomous as autonomous_package
from fplinux_cli.packaging.readme import append_source_companion
from fplinux_cli.workspace import build_inputs
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests import ROOT
from tests.bundle_support import file_record
from tests.small.cli.autonomous_package_fixtures import (
    ArchivePhone,
    write_archive_file,
)
from tests.small.cli.autonomous_package_fixtures import (
    archive_inputs as archive_inputs,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from pathlib import Path

    from tests.small.cli.autonomous_package_fixtures import ArchiveInputs


@pytest.fixture
def ecc_source_download(archive_inputs: ArchiveInputs) -> dict[str, bytes]:
    """Provide a locked local source archive, retaining real projection and license delivery."""
    contents = {
        "src/bch.c": b"synthetic BCH implementation\n",
        "src/bch.h": b"synthetic BCH header\n",
        "COPYING": b"synthetic GPL-2.0-only license\n",
    }
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz") as archive:
        for name, payload in contents.items():
            member = tarfile.TarInfo(f"python-bchlib-controlled/{name}")
            member.size = len(payload)
            archive.addfile(member, io.BytesIO(payload))
    raw = stream.getvalue()
    root = archive_inputs.root
    write_archive_file(root, ".cache/downloads/bch.tar.gz", raw)
    digests = {name: hashlib.sha256(payload).hexdigest() for name, payload in contents.items()}
    write_archive_file(
        root,
        "sources.lock.toml",
        (
            '[bch]\ncommit = "controlled"\n'
            'archive_url = "https://example.invalid/bch.tar.gz"\n'
            f'archive_sha256 = "{hashlib.sha256(raw).hexdigest()}"\n'
            "[bch.files]\n"
            f'bch_c_sha256 = "{digests["src/bch.c"]}"\n'
            f'bch_h_sha256 = "{digests["src/bch.h"]}"\n'
            f'copying_sha256 = "{digests["COPYING"]}"\n'
        ).encode(),
    )
    return contents


def _archive(root: Path) -> tuple[Path, dict[str, bytes], dict[str, int]]:
    archive = next(
        path
        for path in (root / ".cache/out/candidates").glob("*.zip")
        if not path.name.endswith("-sources.zip")
    )
    with zipfile.ZipFile(archive) as source:
        prefix = source.namelist()[0].partition("/")[0] + "/"
        payload = {name.removeprefix(prefix): source.read(name) for name in source.namelist()}
        modes = {
            name.removeprefix(prefix): source.getinfo(name).external_attr >> 16
            for name in source.namelist()
        }
    return archive, payload, modes


@pytest.mark.parametrize("host", ["linux", "windows10"])
def test_autonomous_readme_preserves_complete_host_instructions_and_source_footer(
    host: str,
) -> None:
    config = {
        "identity": {"brand": "ACME", "product": "Demo", "hardware_codes": ["D-1"]},
        "adapter": {"boot_instructions": "Hold the left soft key while connecting USB."},
        "linux": {"root": {"kind": "initramfs"}},
    }
    documents = ROOT / "tests/small/cli/fixtures/package-readme"
    readme = autonomous_package._readme(config, host=host, preparation=True)  # noqa: SLF001 -- compare the delivered document.

    assert readme == (documents / f"autonomous-initramfs-{host}.txt").read_bytes()
    with_sources = append_source_companion(
        readme, "FPLinux-demo-phone-release-autonomous-1234567890abcdef-sources"
    )
    assert with_sources == (documents / f"autonomous-initramfs-{host}-sources.txt").read_bytes()


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
@pytest.mark.parametrize("host", ["linux", "windows10"])
@pytest.mark.usefixtures("ecc_source_download")
def test_autonomous_zip_preserves_receipts_omits_personal_data_and_is_deterministic(
    archive_inputs: ArchiveInputs,
    tmp_path_factory: pytest.TempPathFactory,
    host: str,
) -> None:
    """Recipients get the fixed-phone runtime, complete checksums and no builder capsule."""
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
    with zipfile.ZipFile(companion) as source:
        source_prefix = source.namelist()[0].partition("/")[0] + "/"
        source_payload = {
            name.removeprefix(source_prefix): source.read(name) for name in source.namelist()
        }
    assert source_payload["project/source.c"] == b"controlled corresponding source\n"
    assert json.loads(source_payload["DISTRIBUTION.json"]) == {
        "archive": archive.name,
        "archive_sha256": hashlib.sha256(original).hexdigest(),
    }
    source_checksums = {
        relative: digest
        for digest, relative in (
            line.split("  ", 1) for line in source_payload["SHA256SUMS"].decode().splitlines()
        )
    }
    assert set(source_checksums) == set(source_payload) - {"SHA256SUMS"}
    assert all(
        hashlib.sha256(source_payload[name]).hexdigest() == digest
        for name, digest in source_checksums.items()
    )
    build = json.loads(payload["build-manifest.json"])
    runtime = json.loads(payload["runtime-manifest.json"])
    assert build["device_identity"] == "f" * 64
    assert set(build["files"]) == set(payload) - {"build-manifest.json", "SHA256SUMS"}
    for name, record in build["files"].items():
        assert record == {
            "mode": modes[name] & 0o777,
            "size": len(payload[name]),
            "sha256": hashlib.sha256(payload[name]).hexdigest(),
        }
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
        hashlib.sha256(payload[name]).hexdigest() == digest
        for name, digest in runtime["sha256"].items()
    )
    assert not any(name.startswith(("apks/", "docs/", "sources/")) for name in payload)
    readme = " ".join(payload["README.txt"].decode().split())
    assert "unmount all data-card filesystems" in readme
    assert "docs/use/power/index.html" not in readme
    assert "host/fplinux-usb-keyboard" not in payload
    assert "device-data.cpio" not in payload
    assert "pinmap.bin" not in payload
    assert "keymap.bin" not in payload
    assert not any(name.startswith(".cache/") for name in payload)
    assert b"BUILDER PERSONAL DATA MUST STAY PRIVATE" not in b"".join(payload.values())
    assert b"BUILDER PERSONAL PINS MUST STAY PRIVATE" not in b"".join(payload.values())
    assert b"BUILDER PERSONAL KEYS MUST STAY PRIVATE" not in b"".join(payload.values())
    assert "device-data-manifest.json" not in payload
    assert "board-maps" not in runtime["device_data"]["groups"]
    assert modes["fplinux.py"] == 0o100755
    assert not any(name.startswith("runner/") for name in payload)
    assert "Run.py" not in payload
    assert "platforms/ums9117/host/stock_image.py" not in payload
    assert "tools/fphelper_t117" not in payload
    launcher = "Run.sh" if host == "linux" else "Run.cmd"
    assert launcher in payload
    assert not {"SSH.sh", "SSH.cmd", "prepare.py"}.intersection(payload)
    checksums = {
        relative: digest
        for digest, relative in (
            line.split("  ", 1) for line in payload["SHA256SUMS"].decode().splitlines()
        )
    }
    assert set(checksums) == set(payload) - {"SHA256SUMS"}
    assert all(
        hashlib.sha256(payload[name]).hexdigest() == digest for name, digest in checksums.items()
    )
    if host == "windows10":
        windows = json.loads(payload["windows-host.json"])
        assert windows["host_tools"]["loader"] == "tools/spd_dump.exe"
        assert windows["host_tools"]["driver"] == "tools/driver/wdi-simple.exe"
        assert payload["tools/driver/wdi-simple.exe"] == b"prepared driver helper"
        assert modes["tools/driver/wdi-simple.exe"] == 0o100755
        assert (
            payload["licenses/windows-driver/License.rtf"]
            == b"synthetic driver redistribution license\n"
        )
        for name in inputs.native_files:
            hashes = {**runtime["sha256"], **windows["sha256"]}
            assert hashes[name] == hashlib.sha256(payload[name]).hexdigest()
        assert not any(name.startswith("host/") for name in payload)
        assert "60-fplinux.rules" not in payload
        assert inputs.windows_scripts
        for name, contents in inputs.windows_scripts.items():
            assert payload[name] == contents
            assert runtime["sha256"][name] == hashlib.sha256(contents).hexdigest()
            assert (inputs.root / name).read_bytes() == contents
    else:
        assert not any(name.startswith("common/windows/") for name in payload)
        assert payload["60-fplinux.rules"] == (inputs.bundle / "60-fplinux.rules").read_bytes()
        assert (
            runtime["sha256"]["60-fplinux.rules"]
            == hashlib.sha256(payload["60-fplinux.rules"]).hexdigest()
        )
    assert {
        name: source.read_bytes() for name, source in inputs.native_files.items()
    } == original_inputs
    extracted = tmp_path_factory.mktemp("archive-recipient")
    with zipfile.ZipFile(archive) as source:
        source.extractall(extracted)  # noqa: S202 -- test-owned ZIP built from controlled inputs.
    archive_root = next(extracted.iterdir())
    if host == "windows10":
        from fplinux_cli.runtime import bundle as bundle_runtime  # noqa: PLC0415

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
    runtime_imports = subprocess.run(
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
    assert runtime_imports.returncode == 0, runtime_imports.stderr
    assert json.loads(runtime_imports.stdout) == {
        "target": inputs.target,
        "ecc": inputs.target == "bq-2830l-tank-connect-max",
    }
    write_archive_file(
        inputs.root,
        "scripts/fplinux_cli/runtime/workflow.py",
        b"raise RuntimeError('new checkout code must not enter an old bundle')\n",
    )
    write_archive_file(inputs.root, "common/60-fplinux.rules", b"different checkout USB rules\n")
    package.package_target(inputs.target, autonomous=True, host=host, candidate=True)
    assert archive.read_bytes() == original


@pytest.mark.parametrize("archive_inputs", ["bq-2830l-tank-connect-max"], indirect=True)
@pytest.mark.parametrize("host", ["linux", "windows10"])
def test_declared_ecc_archive_delivers_its_corrector_and_sources(
    archive_inputs: ArchiveInputs, ecc_source_download: dict[str, bytes], host: str
) -> None:
    """Native preparation is replaced; real package collection retains correction inputs."""
    inputs = archive_inputs
    package.package_target(inputs.target, autonomous=True, host=host, candidate=True, offline=True)
    archive, payload, modes = _archive(inputs.root)
    original = archive.read_bytes()
    manifest = json.loads(payload["runtime-manifest.json"])
    assert manifest["nand"]["ecc_format"] == "ums9117-bch12"
    assert manifest["nand"]["raw_page_bytes"] == 2176
    assert (
        payload["platforms/ums9117/host/nand_ecc.py"]
        == (inputs.root / "platforms/ums9117/host/nand_ecc.py").read_bytes()
    )
    tool = "tools/ums9117-nand-ecc.exe" if host == "windows10" else "host/ums9117-nand-ecc"
    assert payload[tool] == b"prepared BCH decoder"
    assert modes[tool] == 0o100755
    assert "device-data.cpio" not in payload
    assert not any(name.startswith(".cache/") for name in payload)
    if host == "linux":
        assert (
            payload["licenses/host-tools/ums9117-nand-ecc-COPYING.txt"]
            == ecc_source_download["COPYING"]
        )
        companion = next((inputs.root / ".cache/out/candidates").glob("*-sources.zip"))
        with zipfile.ZipFile(companion) as archive_source:
            member = next(
                name
                for name in archive_source.namelist()
                if name.endswith("/sources/ums9117-nand-ecc-source.zip")
            )
            source_bytes = archive_source.read(member)
        with zipfile.ZipFile(io.BytesIO(source_bytes)) as source:
            assert source.read("ums9117-nand-ecc/bch.c") == b"synthetic BCH implementation\n"
            assert source.read("ums9117-nand-ecc/COPYING") == ecc_source_download["COPYING"]
            assert (
                source.read("ums9117-nand-ecc/Makefile")
                == (inputs.root / "platforms/ums9117/host/nand-ecc/Makefile").read_bytes()
            )
            assert (
                source.read("ums9117-nand-ecc/fplinux-cli.h")
                == (inputs.root / "include/fplinux/fplinux-cli.h").read_bytes()
            )
    else:
        companion = next((inputs.root / ".cache/out/candidates").glob("*-sources.zip"))
        with zipfile.ZipFile(companion) as source:
            member = next(
                name
                for name in source.namelist()
                if name.endswith("/sources/windows-tools-source.zip")
            )
            assert source.read(member) == b"prepared compiler source projection"
        assert (
            payload["licenses/windows-host/ums9117-nand-ecc-COPYING.txt"]
            == b"prepared BCH license"
        )
        windows = json.loads(payload["windows-host.json"])
        assert manifest["sha256"][tool] == hashlib.sha256(b"prepared BCH decoder").hexdigest()
        assert "nand_ecc" not in windows["host_tools"]
    package.package_target(inputs.target, autonomous=True, host=host, candidate=True, offline=True)
    assert archive.read_bytes() == original


@pytest.mark.parametrize("host", ["linux", "windows10"])
@pytest.mark.parametrize("document", ["readme", "license"])
def test_autonomous_document_changes_preserve_phone_qualification_and_final_checksums(
    archive_inputs: ArchiveInputs,
    capsys: pytest.CaptureFixture[str],
    host: str,
    document: str,
) -> None:
    """Changed delivered instructions and notices retain the executable release receipt."""
    inputs = archive_inputs
    package.package_target(inputs.target, autonomous=True, host=host, candidate=True)
    receipt = next(
        line.removeprefix("Phone-test payload SHA256: ")
        for line in capsys.readouterr().out.splitlines()
        if line.startswith("Phone-test payload SHA256: ")
    )
    _candidate, original, _modes = _archive(inputs.root)
    (inputs.root / "releases.lock.toml").write_text(
        f'[verified]\n{inputs.target} = "{receipt}"\n', encoding="utf-8"
    )
    if document == "readme":
        config = targets.load_target(inputs.target)
        config["adapter"]["boot_instructions"] += " Keep the USB cable connected."
        changed_file = "README.txt"
    elif host == "windows10":
        changed_file = "licenses/windows-driver/License.rtf"
        inputs.native_files[changed_file].write_bytes(b"Revised driver redistribution notice\n")
    else:
        changed_file = "licenses/host-tools/Unlicense.txt"
        (inputs.root / "LICENSES/Unlicense.txt").write_bytes(b"Revised tool license notice\n")

    package.package_target(inputs.target, autonomous=True, host=host)

    assert f"Phone-test payload SHA256: {receipt}\n" in capsys.readouterr().out
    release = next(
        path
        for path in (inputs.root / ".cache/out/releases").glob("*.zip")
        if not path.name.endswith("-sources.zip")
    )
    with zipfile.ZipFile(release) as archive:
        prefix = archive.namelist()[0].partition("/")[0] + "/"
        contents = {name.removeprefix(prefix): archive.read(name) for name in archive.namelist()}
    assert contents[changed_file] != original[changed_file]
    checksums = {
        relative: digest
        for digest, relative in (
            line.split("  ", 1) for line in contents["SHA256SUMS"].decode().splitlines()
        )
    }
    assert checksums[changed_file] == hashlib.sha256(contents[changed_file]).hexdigest()
    manifest = json.loads(contents["build-manifest.json"])
    assert manifest["files"][changed_file]["sha256"] == checksums[changed_file]
    assert manifest["files"][changed_file]["size"] == len(contents[changed_file])


@pytest.mark.parametrize(
    ("host", "changed_file"),
    [
        ("linux", "runtime/bin/python3.14"),
        ("linux", "runtime/lib/ld-musl-x86_64.so.1"),
        ("windows10", "runtime/python314.zip"),
        ("windows10", "tools/driver/wdi-simple.exe"),
    ],
)
def test_autonomous_runtime_change_requires_new_phone_qualification(
    archive_inputs: ArchiveInputs,
    capsys: pytest.CaptureFixture[str],
    host: str,
    changed_file: str,
) -> None:
    """A prior Linux or Windows payload receipt cannot qualify a changed native runtime."""
    inputs = archive_inputs
    package.package_target(inputs.target, autonomous=True, host=host, candidate=True)
    receipt = next(
        line.removeprefix("Phone-test payload SHA256: ")
        for line in capsys.readouterr().out.splitlines()
        if line.startswith("Phone-test payload SHA256: ")
    )
    (inputs.root / "releases.lock.toml").write_text(
        f'[verified]\n{inputs.target} = "{receipt}"\n', encoding="utf-8"
    )
    native_files = inputs.linux_files if host == "linux" else inputs.native_files
    native_files[changed_file].write_bytes(b"different native runtime dependency")
    with pytest.raises(SystemExit, match="not phone-tested"):
        package.package_target(inputs.target, autonomous=True, host=host)


def test_autonomous_source_pairing_distinguishes_build_metadata_with_identical_files(
    archive_inputs: ArchiveInputs,
) -> None:
    """A new build receipt gets its own companion without colliding with an old pair."""
    inputs = archive_inputs
    package.package_target(inputs.target, autonomous=True, candidate=True)
    staging = create_bundle_staging(inputs.root / ".cache/out", inputs.target)
    shutil.copytree(inputs.bundle, staging, dirs_exist_ok=True)
    manifest = json.loads(inputs.manifest)
    manifest.pop("generation")
    manifest["rootfs_receipt"]["recipe"] = "7" * 64
    generation = hashlib.sha256(canonical_json_bytes(manifest)).hexdigest()
    manifest["generation"] = generation
    (staging / "build-manifest.json").write_bytes(canonical_json_bytes(manifest))
    bundle = publish_bundle_generation(
        inputs.root / ".cache/out", inputs.target, staging, generation
    )
    publish_current_bundle(inputs.root / ".cache/out", inputs.target, bundle)

    package.package_target(inputs.target, autonomous=True, candidate=True)

    destination = inputs.root / ".cache/out/candidates"
    companions = list(destination.glob("*-sources.zip"))
    assert len(companions) == 2
    paired_archives: set[str] = set()
    for companion in companions:
        with zipfile.ZipFile(companion) as source:
            prefix = source.namelist()[0].partition("/")[0] + "/"
            distribution = json.loads(source.read(prefix + "DISTRIBUTION.json"))
        archive_path = destination / distribution["archive"]
        assert (
            distribution["archive_sha256"] == hashlib.sha256(archive_path.read_bytes()).hexdigest()
        )
        with zipfile.ZipFile(archive_path) as archive:
            prefix = archive.namelist()[0].partition("/")[0] + "/"
            assert companion.name.encode() in archive.read(prefix + "README.txt")
        paired_archives.add(archive_path.name)
    assert len(paired_archives) == 2


def test_windows_and_offline_options_require_autonomous() -> None:
    """The original Linux package does not silently add dependency preparation."""
    with pytest.raises(SystemExit, match="requires --autonomous"):
        package.package_target("nokia-ta1618", host="windows10")
    with pytest.raises(SystemExit, match="requires --autonomous"):
        package.package_target("nokia-ta1618", offline=True)


def test_autonomous_rejects_the_microsd_context(monkeypatch: pytest.MonkeyPatch) -> None:
    """A RAM data capsule cannot be packaged for a system-card boot."""
    monkeypatch.setattr(
        targets,
        "load_target",
        lambda *_args, **_kwargs: {
            "platform": "ums9117",
            "linux": {"root": {"kind": "microsd-ext4"}},
        },
    )
    with pytest.raises(SystemExit, match="only the RAM boot context"):
        package.package_target("nokia-ta1618", autonomous=True, candidate=True)


def test_sc9820e_rejects_windows_delivery_before_collecting_inputs(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The Linux adapter cannot promise a Windows USB implementation."""
    monkeypatch.setattr(targets, "load_target", lambda *_args, **_kwargs: {"platform": "sc9820e"})
    collect = mock.Mock(side_effect=AssertionError("archive collection must not start"))
    monkeypatch.setattr(package, "load_release_manifest", collect)
    with pytest.raises(SystemExit, match="SC9820E archives support Linux x86-64 only"):
        package.package_target(
            "xiaomi-qin-1s-plus", autonomous=True, host="windows10", candidate=True
        )
    collect.assert_not_called()


@pytest.mark.parametrize("archive_inputs", ["xiaomi-qin-1s-plus"], indirect=True)
def test_sc9820e_linux_archive_delivers_loaders_and_starts_without_phone_data(
    archive_inputs: ArchiveInputs, tmp_path: Path
) -> None:
    """The delivered launcher imports in isolation and all three stage bytes stay admitted."""
    inputs = archive_inputs
    package.package_target(
        inputs.target, autonomous=True, host="linux", candidate=True, offline=True
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
    with zipfile.ZipFile(archive) as source:
        source.extractall(extracted)  # noqa: S202 -- owned ZIP built from controlled inputs.
    archive_root = next(extracted.iterdir())
    help_result = subprocess.run(
        [sys.executable, "-I", str(archive_root / "fplinux.py"), "run", "--help"],
        cwd=extracted,
        env={},
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    assert help_result.returncode == 0, help_result.stderr
    assert runtime["device_data"] is None
    assert runtime["nand"] is None
    assert "--loader-inputs" not in help_result.stdout


@pytest.mark.parametrize("archive_inputs", ["xiaomi-qin-1s-plus"], indirect=True)
def test_sc9820e_autonomous_archive_requires_a_build_without_personal_device_data(
    archive_inputs: ArchiveInputs, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest = json.loads(archive_inputs.manifest)
    context = ReleaseContext(
        manifest["release_id"], manifest["source_commit"], manifest["source_snapshot"]
    )

    def package_snapshot(
        *_arguments: object, include_device_data: bool = True, **_options: object
    ) -> WorkspaceSnapshot:
        """Replace source capture with distinct owner-data and clean build identities."""
        return WorkspaceSnapshot((), ("a" if include_device_data else "9") * 64)

    monkeypatch.setattr(build_inputs, "target_workspace_snapshot", package_snapshot)
    with pytest.raises(SystemExit, match="build output is stale") as rejected:
        package.package_target(inputs.target, autonomous=True, candidate=True, offline=True)
    assert "./fplinux build xiaomi-qin-1s-plus --build-type release --without-device-data" in str(
        rejected.value
    )
    assert not (inputs.root / ".cache/out/candidates").exists()

    staging = create_bundle_staging(inputs.root / ".cache/out", inputs.target)
    shutil.copytree(inputs.bundle, staging, dirs_exist_ok=True)
    image = staging / "image/ramboot.bin"
    image.write_bytes(b"clean source-built RAM payload\n")
    runtime_path = staging / "runtime-manifest.json"
    runtime = json.loads(runtime_path.read_bytes())
    runtime["sha256"]["image/ramboot.bin"] = hashlib.sha256(image.read_bytes()).hexdigest()
    runtime_path.write_bytes(canonical_json_bytes(runtime))
    manifest = json.loads(inputs.manifest)
    manifest.pop("generation")
    manifest["workspace_digest"] = "9" * 64
    manifest["files"]["image/ramboot.bin"] = file_record(image)
    manifest["files"]["runtime-manifest.json"] = file_record(runtime_path)
    generation = hashlib.sha256(canonical_json_bytes(manifest)).hexdigest()
    manifest["generation"] = generation
    (staging / "build-manifest.json").write_bytes(canonical_json_bytes(manifest))
    clean_bundle = publish_bundle_generation(
        inputs.root / ".cache/out", inputs.target, staging, generation
    )
    publish_current_bundle(inputs.root / ".cache/out", inputs.target, clean_bundle)

    package.package_target(inputs.target, autonomous=True, candidate=True, offline=True)
    _archive_path, payload, _modes = _archive(inputs.root)
    assert payload["image/ramboot.bin"] == b"clean source-built RAM payload\n"


def test_package_parser_keeps_linux_default_and_selects_the_requested_host() -> None:
    """CLI host options do not change the default raw Linux archive."""
    parser, _check = create_parser(("nokia-ta1618",))
    default = parser.parse_args(["package", "nokia-ta1618"])
    assert (default.autonomous, default.host, default.offline) == (False, "linux", False)
    windows = parser.parse_args(
        ["package", "nokia-ta1618", "--autonomous", "--host", "windows10", "--offline"]
    )
    assert (windows.autonomous, windows.host, windows.offline) == (True, "windows10", True)


@pytest.mark.parametrize(
    "archive_inputs",
    [ArchivePhone("nokia-ta1618", required_phone_data=False)],
    indirect=True,
)
@pytest.mark.parametrize("host", ["linux", "windows10"])
def test_headless_ums_archive_imports_without_preparation_and_creates_a_transport_capsule(
    archive_inputs: ArchiveInputs, tmp_path: Path, host: str
) -> None:
    """The projected package admits and delivers the empty-capsule dependency closure."""
    cpio = shutil.which("cpio")
    if cpio is None:
        pytest.fail("GNU cpio is required in the declared test environment")
    inputs = archive_inputs
    package.package_target(inputs.target, autonomous=True, host=host, candidate=True)
    archive, payload, modes = _archive(inputs.root)
    runtime = json.loads(payload["runtime-manifest.json"])
    assert runtime["device_data"] is None
    assert "scripts/fplinux_cli/device_data/prepare.py" not in payload
    assert "backup" not in payload["README.txt"].decode()
    recipient = tmp_path / "recipient"
    recipient.mkdir()
    with zipfile.ZipFile(archive) as source:
        source.extractall(recipient)  # noqa: S202 -- test-owned ZIP from controlled inputs.
    archive_root = next(recipient.iterdir())
    for relative, mode in modes.items():
        (archive_root / relative).chmod(mode & 0o777)
    capsule = tmp_path / "transport.cpio"
    imported = subprocess.run(
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
    assert imported.returncode == 0, imported.stderr
    assert json.loads(imported.stdout) == {"target": inputs.target, "preparation": None}
    capsule_root = tmp_path / "capsule"
    capsule_root.mkdir()
    extracted = subprocess.run(
        [
            cpio,
            "--extract",
            "--make-directories",
            "--no-absolute-filenames",
            "--no-preserve-owner",
        ],
        input=capsule.read_bytes(),
        cwd=capsule_root,
        env={},
        capture_output=True,
        timeout=10,
        check=False,
    )
    assert extracted.returncode == 0, extracted.stderr.decode()
    assert {
        path.relative_to(capsule_root).as_posix(): path.read_bytes()
        for path in capsule_root.rglob("*")
        if path.is_file()
    } == {"etc/fplinux/usb-transport": b"rndis\n"}


def test_runtime_device_data_descriptor_records_the_selected_audio_parameters() -> None:
    """The archive snapshot supplies identity and policy to standalone preparation."""
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

    snapshot = device_data_descriptor("example-phone", config)

    assert snapshot is not None
    assert set(snapshot) == {"parser", "groups", "audio_policy", "ecc_module"}
    assert snapshot["parser"] == "targets/example-phone/example_device_data.py"
    assert snapshot["audio_policy"] == {
        "speaker_vibration": False,
        "speaker_from_headphones": True,
        "headfree_headphones_only": False,
    }


@pytest.mark.parametrize(
    ("platform", "root_kind", "groups"),
    [
        pytest.param("sc9820e", "initramfs", {"wcn-firmware": []}, id="source-loader"),
        pytest.param("ums9117", "external", {"audio-profile": []}, id="system-card"),
        pytest.param("ums9117", "initramfs", {}, id="headless"),
    ],
)
def test_runtime_without_required_ram_data_has_no_preparation_descriptor(
    platform: str, root_kind: str, groups: dict[str, list[object]]
) -> None:
    """Direct boot and built rootfs workflows do not trigger NAND preparation."""
    descriptor = device_data_descriptor(
        "demo",
        {
            "platform": platform,
            "linux": {"root": {"kind": root_kind}},
            "device_data": {"parser": None, "groups": groups},
        },
    )

    assert descriptor is None
