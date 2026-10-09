# SPDX-License-Identifier: GPL-2.0-only
"""Archive behavior with synthetic built files and prepared native-tool boundaries."""

from __future__ import annotations

import hashlib
import io
import json
import shutil
import subprocess
import sys
import tarfile
import zipfile
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest
from fplinux_cli import common
from fplinux_cli.alpine import signing
from fplinux_cli.artifacts.bundles import publish_current_bundle
from fplinux_cli.build import linux_host, windows_driver
from fplinux_cli.build import windows as windows_build
from fplinux_cli.cli import autonomous_package, bundles, package
from fplinux_cli.cli.parser import create_parser
from fplinux_cli.common import canonical_json_bytes
from fplinux_cli.environment import image_state, images
from fplinux_cli.environment.image_state import ImageState
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.workspace import build_inputs
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests import ROOT
from tests.bundle_support import file_record
from tests.small.cli.package_docs_fixtures import write_offline_export

if TYPE_CHECKING:
    from pathlib import Path


@dataclass(frozen=True)
class ArchiveInputs:
    """Synthetic bundle and prepared tools owned by one archive case."""

    root: Path
    target: str
    bundle: Path
    manifest: bytes
    native_files: dict[str, Path]
    windows_scripts: dict[str, bytes]


def _write(root: Path, relative: str, contents: bytes, *, mode: int = 0o644) -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(contents)
    path.chmod(mode)
    return path


@pytest.fixture
def archive_inputs(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, request: pytest.FixtureRequest
) -> ArchiveInputs:
    """Replace compilation and host identity, retaining real collection and ZIP writing."""
    source = common.ROOT
    target = getattr(request, "param", "nokia-ta1618")
    config = targets.load_target(target)
    platform = platforms.load_platform(config["platform"])
    release = releases.load_release(target)
    for relative in (
        "LICENSE",
        "LICENSES/Unlicense.txt",
        "THIRD_PARTY_LICENSES/musl/COPYRIGHT",
        "common/60-fplinux.rules",
        "common/autonomous.py",
        "common/__init__.py",
        "common/prepare.py",
        "common/windows_host.py",
        "common/Run.sh",
        "common/Run.cmd",
        "platforms/ums9117/host/stock_image.py",
        "platforms/ums9117/host/nand_ecc.py",
        "platforms/ums9117/host/nand-ecc/Makefile",
        "platforms/ums9117/host/nand-ecc/ums9117-nand-ecc.c",
        "include/fplinux/fplinux-cli.h",
        "lib/fplinux/fplinux-cli.c",
        f"targets/{target}/{config['device_data']['parser']}",
        f"targets/{target}/release/README.txt",
    ):
        _write(tmp_path, relative, (source / relative).read_bytes())
    shutil.copytree(source / "scripts/fplinux_cli", tmp_path / "scripts/fplinux_cli")
    windows_scripts = {}
    for resource in sorted((source / "common/windows").rglob("*.ps1")):
        relative = resource.relative_to(source).as_posix()
        contents = resource.read_bytes()
        windows_scripts[relative] = contents
        _write(tmp_path, relative, contents)
    public_key = signing.signing_public_key(tmp_path / ".cache")
    _write(tmp_path, public_key.relative_to(tmp_path).as_posix(), b"test public signing key\n")
    signing_digest = hashlib.sha256(public_key.read_bytes()).hexdigest()
    generation = "c" * 64
    bundle = tmp_path / ".cache/out" / target / "builds/release/bundles" / generation
    normalized = package.load_release_manifest(target, config)
    for relative in normalized["bundle_files"]:
        contents = (
            b"BUILDER PERSONAL DATA MUST STAY PRIVATE\n"
            if relative == "device-data.cpio"
            else f"synthetic built file: {relative}\n".encode()
        )
        mode = 0o755 if relative in normalized["executables"] else 0o644
        _write(bundle, relative, contents, mode=mode)
    snapshot = WorkspaceSnapshot((), "a" * 64)
    manifest = canonical_json_bytes(
        {
            "rootfs_receipt": {"recipe": "d" * 64, "sha256": "e" * 64},
            "boot_artifacts": {"required": []},
            "container_image_recipe": "b" * 64,
            "container_image_content": "c" * 64,
            "apk_signing_key": signing_digest,
            "device_identity": "f" * 64,
            "files": {
                relative: file_record(bundle / relative) for relative in normalized["bundle_files"]
            },
            "generation": generation,
            "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
            "linux_recipe": "2" * 64,
            "profile": None,
            "build_type": "release",
            "target": target,
            "workspace_digest": snapshot.recipe,
        }
    )
    _write(bundle, "build-manifest.json", manifest)
    publish_current_bundle(tmp_path / ".cache/out", target, bundle)
    native_files = {
        relative: _write(tmp_path / "prepared-native", relative, relative.encode())
        for relative in (
            "tools/spd_dump.exe",
            "tools/libc_server.exe",
            "tools/fphelper_t117.exe",
            "tools/libusb-1.0.dll",
            "tools/openssh/ssh.exe",
            "tools/openssh/ssh-keygen.exe",
            "tools/openssh/ssh-keyscan.exe",
            "runtime/python.exe",
            "runtime/python314.zip",
        )
    }
    driver_files = {
        "tools/driver/wdi-simple.exe": _write(
            tmp_path / "prepared-driver", "tools/driver/wdi-simple.exe", b"prepared driver helper"
        ),
        "licenses/windows-driver/License.rtf": _write(
            tmp_path / "prepared-driver",
            "licenses/windows-driver/License.rtf",
            b"synthetic driver redistribution license\n",
        ),
    }
    linux_files = {
        relative: _write(
            tmp_path / "prepared-linux", relative, b"prepared Linux runtime", mode=0o755
        )
        for relative in ("runtime/bin/python3.14", "runtime/lib/ld-musl-x86_64.so.1")
    }

    def prepared_native(**_options: object) -> dict[str, Path]:
        """Stand in for the separate, expensive cross-compilation boundary."""
        files = dict(native_files)
        if _options.get("nand_ecc"):
            files["tools/ums9117-nand-ecc.exe"] = _write(
                tmp_path / "prepared-native", "tools/ums9117-nand-ecc.exe", b"prepared BCH decoder"
            )
            files["sources/windows-tools-source.zip"] = _write(
                tmp_path / "prepared-native",
                "sources/windows-tools-source.zip",
                b"prepared compiler source projection",
            )
            files["licenses/windows-host/ums9117-nand-ecc-COPYING.txt"] = _write(
                tmp_path / "prepared-native",
                "licenses/windows-host/ums9117-nand-ecc-COPYING.txt",
                b"prepared BCH license",
            )
        return files

    def prepared_driver(**_options: object) -> dict[str, Path]:
        """Stand in for the locked driver build and redistributable preparation."""
        return dict(driver_files)

    def prepared_linux(**_options: object) -> dict[str, Path]:
        """Stand in for the exact pinned-image runtime export boundary."""
        return dict(linux_files)

    monkeypatch.setattr(common, "ROOT", tmp_path)
    monkeypatch.setattr(targets, "load_target", lambda *_args, **_kwargs: config)
    monkeypatch.setattr(releases, "load_release", lambda *_args, **_kwargs: release)
    monkeypatch.setattr(platforms, "load_platform", lambda *_args: platform)
    monkeypatch.setattr(
        build_inputs, "target_workspace_snapshot", lambda *_args, **_kwargs: snapshot
    )
    monkeypatch.setattr(images, "container_image_recipe_digest", lambda: "b" * 64)
    monkeypatch.setattr(
        image_state, "load_image_state", lambda *_args: ImageState("b" * 64, "c" * 64, "c" * 64)
    )
    monkeypatch.setattr(
        bundles,
        "build_identity",
        lambda *_args: bundles.BuildIdentity(snapshot.recipe, "b" * 64, "c" * 64, signing_digest),
    )
    monkeypatch.setattr(
        autonomous_package,
        "_linux_host_tool",
        lambda _target, _platform, tool, **_kwargs: (
            b"extractor" if tool == "fphelper_t117" else b"prepared BCH decoder"
        ),
    )
    monkeypatch.setattr(windows_build, "prepare_windows_host", prepared_native)
    monkeypatch.setattr(windows_driver, "prepare_windows_driver", prepared_driver)
    monkeypatch.setattr(linux_host, "prepare_linux_host", prepared_linux)
    write_offline_export(tmp_path)
    return ArchiveInputs(
        tmp_path, target, bundle, manifest, {**native_files, **driver_files}, windows_scripts
    )


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
    _write(root, ".cache/downloads/bch.tar.gz", raw)
    digests = {name: hashlib.sha256(payload).hexdigest() for name, payload in contents.items()}
    _write(
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
    archive = next((root / ".cache/out/candidates").glob("*.zip"))
    with zipfile.ZipFile(archive) as source:
        prefix = source.namelist()[0].partition("/")[0] + "/"
        payload = {name.removeprefix(prefix): source.read(name) for name in source.namelist()}
        modes = {
            name.removeprefix(prefix): source.getinfo(name).external_attr >> 16
            for name in source.namelist()
        }
    return archive, payload, modes


@pytest.mark.parametrize(
    "archive_inputs",
    [
        "nokia-ta1618",
        "inoi-240-modern-4g",
        "inoi-244-modern-4g",
        "maxvi-k15n-4g",
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
    assert payload["build-manifest.json"] == inputs.manifest
    assert (
        payload["runtime-manifest.json"] == (inputs.bundle / "runtime-manifest.json").read_bytes()
    )
    assert "device-data.cpio" not in payload
    assert not any(name.startswith(".cache/") for name in payload)
    assert b"BUILDER PERSONAL DATA MUST STAY PRIVATE" not in b"".join(payload.values())
    declaration = json.loads(payload["device-data-manifest.json"])
    assert declaration["target"] == inputs.target
    assert declaration["device_data"]["groups"]["board-maps"] == [
        {"source": "pinmap.bin", "destination": "pinmap.bin"},
        {"source": "keymap.bin", "destination": "keymap.bin"},
    ]
    assert modes["Run.py"] == 0o100755
    launcher = "Run.sh" if host == "linux" else "Run.cmd"
    assert launcher in payload
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
            assert windows["sha256"][name] == hashlib.sha256(payload[name]).hexdigest()
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
    runtime_imports = subprocess.run(
        [
            sys.executable,
            "-I",
            str(ROOT / "tests/fixtures/processes/autonomous_runtime_imports.py"),
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
