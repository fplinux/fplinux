# SPDX-License-Identifier: GPL-2.0-only
"""Read independently created ext4 files through the delivered isolated reader."""

from __future__ import annotations

import contextlib
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

import pytest
from fplinux_cli.build.host_tools.phone_data import prepare_python_modules
from fplinux_cli.build.runtime import runtime_source_files
from fplinux_cli.device_data.emmc import EmmcError, FileSelection, extract_files
from fplinux_cli.manifests.targets import load_target

from tests import ROOT
from tests.artifact.test_release_package import ReleaseArchiveInputs, archive_contents
from tests.process import run_process
from tests.small.device_data.emmc_fixtures import write_disk


@dataclass(frozen=True)
class SavedPhone:
    """An actual GPT image, independently generated filesystems and pinned module closure."""

    image: Path
    modules: Path
    system: Path
    userdata: Path


@pytest.fixture(scope="module")
def saved_phone_template(tmp_path_factory: pytest.TempPathFactory) -> SavedPhone:
    """Create shared ext4 inputs with different base and override audio originals."""
    tmp_path = tmp_path_factory.mktemp("saved-phone-template")
    if shutil.which("mke2fs") is None:
        pytest.fail("quality environment lacks mke2fs")
    partitions = []
    for name, path, data in (
        ("system", "etc/audio_para", b"system audio default\n"),
        ("userdata", "local/media/audio_para", b"this phone's selected audio override\n"),
    ):
        tree = tmp_path / name
        original = tree / path
        original.parent.mkdir(parents=True)
        original.write_bytes(data)
        if name == "userdata":
            (original.parent / "linked_audio").symlink_to("audio_para")
        image = tmp_path / f"{name}.ext4"
        with image.open("wb") as output:
            output.truncate(16 * 1024 * 1024)
        result = subprocess.run(
            ["mke2fs", "-q", "-t", "ext4", "-F", "-b", "4096", "-d", str(tree), str(image)],
            capture_output=True,
            check=False,
            timeout=30,
        )
        assert result.returncode == 0, result.stderr.decode()
        partitions.append((name, image))
    image = tmp_path / "phone.bin"
    write_disk(image, partitions)
    delivery = tmp_path / "reader"
    prepare_python_modules(delivery, root=ROOT, cache=Path("/cache"))
    return SavedPhone(image, delivery / "scripts", partitions[0][1], partitions[1][1])


@pytest.fixture
def saved_phone(tmp_path: Path, saved_phone_template: SavedPhone) -> SavedPhone:
    """Give each reader its own image while borrowing the pinned module input."""
    image = tmp_path / "phone.bin"
    shutil.copyfile(saved_phone_template.image, image)
    return SavedPhone(
        image,
        saved_phone_template.modules,
        saved_phone_template.system,
        saved_phone_template.userdata,
    )


def test_isolated_ext4_reader_uses_phone_override_before_system_default(
    saved_phone: SavedPhone,
) -> None:
    before = hashlib.sha256(saved_phone.image.read_bytes()).digest()
    selected = extract_files(
        saved_phone.image,
        [
            FileSelection(
                "audio_para",
                (("userdata", "/local/media/audio_para"), ("system", "/etc/audio_para")),
                100,
            )
        ],
        python_modules=saved_phone.modules,
    )
    assert selected == {"audio_para": b"this phone's selected audio override\n"}
    assert hashlib.sha256(saved_phone.image.read_bytes()).digest() == before


def test_isolated_ext4_reader_falls_back_only_when_first_file_is_absent(
    saved_phone: SavedPhone,
) -> None:
    selected = extract_files(
        saved_phone.image,
        [
            FileSelection(
                "audio_para",
                (("userdata", "/local/media/absent"), ("system", "/etc/audio_para")),
                100,
            )
        ],
        python_modules=saved_phone.modules,
    )
    assert selected == {"audio_para": b"system audio default\n"}


def test_isolated_ext4_reader_does_not_fall_back_from_oversized_override(
    saved_phone: SavedPhone,
) -> None:
    with pytest.raises(EmmcError, match="bounded regular"):
        extract_files(
            saved_phone.image,
            [
                FileSelection(
                    "audio_para",
                    (("userdata", "/local/media/audio_para"), ("system", "/etc/audio_para")),
                    24,
                )
            ],
            python_modules=saved_phone.modules,
        )


def test_isolated_ext4_reader_does_not_follow_a_link_or_fall_back_from_it(
    saved_phone: SavedPhone,
) -> None:
    with pytest.raises(EmmcError, match="bounded regular"):
        extract_files(
            saved_phone.image,
            [
                FileSelection(
                    "audio_para",
                    (("userdata", "/local/media/linked_audio"), ("system", "/etc/audio_para")),
                    100,
                )
            ],
            python_modules=saved_phone.modules,
        )


def test_isolated_ext4_reader_requires_every_selected_original(saved_phone: SavedPhone) -> None:
    with pytest.raises(EmmcError, match="required original vbc_eq"):
        extract_files(
            saved_phone.image,
            [
                FileSelection("audio_para", (("system", "/etc/audio_para"),), 100),
                FileSelection("vbc_eq", (("userdata", "/local/media/vbc_eq"),), 100),
            ],
            python_modules=saved_phone.modules,
        )


@pytest.mark.parametrize("host", ["linux", "windows"])
def test_runtime_archive_extracts_saved_originals_and_imports_target_resources(
    saved_phone: SavedPhone, tmp_path: Path, host: str
) -> None:
    """Each host ZIP carries the same independently usable pure preparation closure."""
    config = load_target("xiaomi-qin-1s-plus")
    sources = runtime_source_files("xiaomi-qin-1s-plus", config, root=ROOT)
    delivery = saved_phone.modules.parent
    sources.update(
        {
            path.relative_to(delivery).as_posix(): path
            for path in delivery.rglob("*")
            if path.is_file()
        }
    )
    inputs = ReleaseArchiveInputs(tmp_path / "packaging")
    inputs.publish_bundle(
        "d" * 64,
        extra_files={relative: source.read_bytes() for relative, source in sources.items()},
    )
    with contextlib.ExitStack() as stack:
        for patch in inputs.package_patches():
            stack.enter_context(patch)
        inputs.package(host=host)
    archive_path = inputs.cache / f"out/releases/FPLinux-nokia-ta1618-{host}-x64-1234567890ab.zip"
    files, _modes = archive_contents(archive_path)
    runtime = json.loads(files["runtime-manifest.json"])
    for relative in sources:
        if relative in files:
            assert runtime["sha256"][relative] == hashlib.sha256(files[relative]).hexdigest()
    extracted = tmp_path / "recipient"
    with zipfile.ZipFile(archive_path) as archive:
        archive.extractall(extracted)
    result = run_process(
        [
            sys.executable,
            "-B",
            "-I",
            "-S",
            str(ROOT / "tests/artifact/fixtures/emmc-files/archived_reader.py"),
            str(extracted / archive_path.stem),
            str(saved_phone.image),
        ],
        name="archived eMMC preparation",
        timeout=30,
        cwd=extracted,
        env={},
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == {
        "original": "this phone's selected audio override\n",
        "rf_config": (b"\x01" + bytes(615)).hex(),
    }
