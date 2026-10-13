# SPDX-License-Identifier: GPL-2.0-only
"""Archive behavior with synthetic built files and prepared native-tool boundaries."""

from __future__ import annotations

import hashlib
import io
import json
import subprocess
import sys
import tarfile
import zipfile
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.cli.parser import create_parser
from fplinux_cli.manifests import targets
from fplinux_cli.packaging import archive as package
from fplinux_cli.packaging import autonomous as autonomous_package
from fplinux_cli.packaging.readme import append_source_companion

from tests import ROOT
from tests.small.cli.autonomous_package_fixtures import (
    archive_inputs as archive_inputs,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.small.cli.autonomous_package_fixtures import (
    write_archive_file,
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
    readme = autonomous_package._readme(config, host=host)  # noqa: SLF001 -- compare the delivered document.

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
    original_inputs = {name: source.read_bytes() for name, source in inputs.native_files.items()}
    package.package_target(inputs.target, autonomous=True, host=host, candidate=True)
    archive, payload, modes = _archive(inputs.root)
    original = archive.read_bytes()
    companion = next((inputs.root / ".cache/out/candidates").glob("*-sources.zip"))
    assert payload["README.txt"].endswith(
        (
            "\nMatching source and build material:\n"
            f"  {companion.name}\n"
            "Keep it available alongside this archive. Licenses remain included here.\n"
        ).encode()
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
    assert (
        build["files"]["runtime-manifest.json"]["sha256"]
        == hashlib.sha256(payload["runtime-manifest.json"]).hexdigest()
    )
    assert set(runtime["host_tools"]) == {"loader", "bridge"}
    assert all(name in payload for name in runtime["sha256"])
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
    declaration = json.loads(payload["device-data-manifest.json"])
    assert declaration["target"] == inputs.target
    assert declaration["device_data"]["groups"]["board-maps"] == [
        {"source": "pinmap.bin", "destination": "pinmap.bin"},
        {"source": "keymap.bin", "destination": "keymap.bin"},
    ]
    assert modes["Run.py"] == 0o100755
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
            assert windows["sha256"][name] == hashlib.sha256(contents).hexdigest()
            assert (inputs.root / name).read_bytes() == contents
    else:
        assert not any(name.startswith("common/windows/") for name in payload)
    assert {
        name: source.read_bytes() for name, source in inputs.native_files.items()
    } == original_inputs
    extracted = tmp_path_factory.mktemp("archive-recipient")
    with zipfile.ZipFile(archive) as source:
        source.extractall(extracted)  # noqa: S202 -- test-owned ZIP built from controlled inputs.
    archive_root = next(extracted.iterdir())
    if host == "windows10":
        from common.windows_host import validate_archive  # noqa: PLC0415

        admitted = validate_archive(archive_root)
        assert admitted.runtime["target"] == inputs.target
    help_result = subprocess.run(
        [sys.executable, "-I", str(archive_root / "Run.py"), "--help"],
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
    manifest = json.loads(payload["device-data-manifest.json"])
    assert manifest["nand"]["ecc_format"] == "ums9117-bch12"
    assert manifest["nand"]["raw_page_bytes"] == 2176
    assert (
        payload["platforms/ums9117/host/nand_ecc.py"]
        == (inputs.root / "platforms/ums9117/host/nand_ecc.py").read_bytes()
    )
    tool = "tools/ums9117-nand-ecc.exe" if host == "windows10" else "tools/ums9117-nand-ecc"
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
        assert windows["sha256"][tool] == hashlib.sha256(b"prepared BCH decoder").hexdigest()
        assert "nand_ecc" not in windows["host_tools"]
    package.package_target(inputs.target, autonomous=True, host=host, candidate=True, offline=True)
    assert archive.read_bytes() == original


@pytest.mark.parametrize("changed_file", ["runtime/python314.zip", "tools/driver/wdi-simple.exe"])
def test_windows_runtime_change_requires_new_phone_qualification(
    archive_inputs: ArchiveInputs, capsys: pytest.CaptureFixture[str], changed_file: str
) -> None:
    """A prior Linux or Windows payload receipt cannot qualify a changed native runtime."""
    inputs = archive_inputs
    package.package_target(inputs.target, autonomous=True, host="windows10", candidate=True)
    receipt = next(
        line.removeprefix("Phone-test payload SHA256: ")
        for line in capsys.readouterr().out.splitlines()
        if line.startswith("Phone-test payload SHA256: ")
    )
    (inputs.root / "releases.lock.toml").write_text(
        f'[verified]\n{inputs.target} = "{receipt}"\n', encoding="utf-8"
    )
    inputs.native_files[changed_file].write_bytes(b"different native runtime dependency")
    with pytest.raises(SystemExit, match="not phone-tested"):
        package.package_target(inputs.target, autonomous=True, host="windows10")


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
        lambda *_args, **_kwargs: {"linux": {"root": {"kind": "microsd-ext4"}}},
    )
    with pytest.raises(SystemExit, match="only the RAM boot context"):
        package.package_target("nokia-ta1618", autonomous=True, candidate=True)


def test_package_parser_keeps_linux_default_and_selects_the_requested_host() -> None:
    """CLI host options do not change the default raw Linux archive."""
    parser, _check = create_parser(("nokia-ta1618",))
    default = parser.parse_args(["package", "nokia-ta1618"])
    assert (default.autonomous, default.host, default.offline) == (False, "linux", False)
    windows = parser.parse_args(
        ["package", "nokia-ta1618", "--autonomous", "--host", "windows10", "--offline"]
    )
    assert (windows.autonomous, windows.host, windows.offline) == (True, "windows10", True)


def test_device_data_manifest_records_the_selected_audio_parameters() -> None:
    """The archive snapshot supplies identity and policy to standalone preparation."""
    config = {
        "platform": "ums9117",
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

    snapshot = json.loads(autonomous_package.device_data_manifest("example-phone", config))

    assert snapshot["machine_compatible"] == "example,phone"
    assert snapshot["audio_policy"] == {
        "speaker_vibration": False,
        "speaker_from_headphones": True,
        "headfree_headphones_only": False,
    }
