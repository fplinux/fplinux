# SPDX-License-Identifier: GPL-2.0-only
"""Check supplemental initramfs artifacts with GNU cpio as an independent reader."""

from __future__ import annotations

import hashlib
import lzma
import os
import shutil
import subprocess
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.device_data.cpio import create_device_data_initramfs
from fplinux_cli.device_data.inputs import FirmwareInput

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def cpio() -> str:
    """Locate the external archive reader used by these artifact checks."""
    executable = shutil.which("cpio")
    if executable is None:
        pytest.skip("GNU cpio is required for archive extraction")
    return executable


def _firmware(destination: str, contents: bytes) -> FirmwareInput:
    return FirmwareInput(
        source=destination.rsplit("/", 1)[-1],
        destination=destination,
        contents=contents,
        sha256=hashlib.sha256(contents).hexdigest(),
    )


def _extract(archive: bytes, directory: Path, cpio: str) -> None:
    extracted = subprocess.run(
        [
            cpio,
            "--extract",
            "--make-directories",
            "--no-absolute-filenames",
            "--no-preserve-owner",
            "--preserve-modification-time",
        ],
        input=archive,
        cwd=directory,
        capture_output=True,
        check=False,
        timeout=10,
    )
    assert extracted.returncode == 0, extracted.stderr.decode()


def test_firmware_archive_extracts_exact_private_data_and_ignores_board_maps(
    tmp_path: Path, cpio: str
) -> None:
    bluetooth = b"controller\x00\x01"
    radio = b"radio\x02"
    profile = b"audio-profile\x00" * 1024
    groups = {
        "bluetooth": (_firmware("chip/controller.bin", bluetooth),),
        "fm-radio": (_firmware("radio.bin", radio),),
        "audio-profile": (_firmware("fplinux/profile.bin", profile),),
        "board-maps": (_firmware("board.bin", b"board configuration"),),
    }

    archive = create_device_data_initramfs(groups)
    _extract(archive, tmp_path, cpio)

    files = {
        path.relative_to(tmp_path).as_posix(): path
        for path in tmp_path.rglob("*")
        if path.is_file()
    }
    assert set(files) == {
        "lib/firmware/chip/controller.bin",
        "lib/firmware/radio.bin",
        "lib/firmware/fplinux/profile.bin.xz",
    }
    assert files["lib/firmware/chip/controller.bin"].read_bytes() == bluetooth
    assert files["lib/firmware/radio.bin"].read_bytes() == radio
    compressed = files["lib/firmware/fplinux/profile.bin.xz"].read_bytes()
    assert len(compressed) < len(profile)
    assert lzma.decompress(compressed, format=lzma.FORMAT_XZ) == profile
    for path in tmp_path.rglob("*"):
        assert path.stat().st_mode & 0o777 == (0o755 if path.is_dir() else 0o600)
        if path.is_file():
            assert path.stat().st_mtime == 0
    assert len(archive) % 512 == 0

    listed = subprocess.run(
        [cpio, "--list", "--verbose", "--numeric-uid-gid"],
        input=archive,
        env={**os.environ, "LC_ALL": "C", "TZ": "UTC"},
        capture_output=True,
        check=False,
        timeout=10,
    )
    assert listed.returncode == 0, listed.stderr.decode()
    assert listed.stdout
    for line in listed.stdout.decode().splitlines():
        assert line.split()[2:4] == ["0", "0"]
        assert line.split()[5:8] == ["Jan", "1", "1970"]


def test_equivalent_group_and_file_orders_produce_identical_archives() -> None:
    first = _firmware("z.bin", b"first")
    second = _firmware("a.bin", b"second")
    profile = _firmware("profile.bin", b"profile")

    forward = create_device_data_initramfs(
        {"bluetooth": (first, second), "audio-profile": (profile,)}
    )
    reverse = create_device_data_initramfs(
        {"audio-profile": (profile,), "bluetooth": (second, first)}
    )

    assert forward == reverse


def test_external_radio_data_reaches_the_capsule_and_calibration_changes_it(
    tmp_path: Path, cpio: str
) -> None:
    image = bytes(range(256)) * 256
    calibration = b"fitted calibration\x00\r\n\x1a"
    mac = b"\x40\x45\xda\x6e\xc5\x0f"
    bluetooth = b"controller settings\x00"
    groups = {
        "wcn-firmware": (
            _firmware("radio/image.bin", image),
            _firmware("radio/calibration.bin", calibration),
            _firmware("radio/mac.bin", mac),
            _firmware("radio/bluetooth.bin", bluetooth),
        ),
    }
    archive = create_device_data_initramfs(groups)
    _extract(archive, tmp_path, cpio)

    firmware = tmp_path / "lib/firmware/radio"
    assert lzma.decompress((firmware / "image.bin.xz").read_bytes()) == image
    assert (firmware / "calibration.bin").read_bytes() == calibration
    assert (firmware / "mac.bin").read_bytes() == mac
    assert (firmware / "bluetooth.bin").read_bytes() == bluetooth
    assert all(path.stat().st_mode & 0o777 == 0o600 for path in firmware.iterdir())

    changed = tmp_path / "changed"
    changed.mkdir()
    groups["wcn-firmware"] = (
        groups["wcn-firmware"][0],
        _firmware("radio/calibration.bin", b"different fitted calibration"),
        *groups["wcn-firmware"][2:],
    )
    changed_archive = create_device_data_initramfs(groups)
    _extract(changed_archive, changed, cpio)

    assert changed_archive != archive
    changed_firmware = changed / "lib/firmware/radio"
    assert (changed_firmware / "calibration.bin").read_bytes() == b"different fitted calibration"
    assert (changed_firmware / "image.bin.xz").read_bytes() == (
        firmware / "image.bin.xz"
    ).read_bytes()
    assert (changed_firmware / "mac.bin").read_bytes() == mac
    assert (changed_firmware / "bluetooth.bin").read_bytes() == bluetooth


def test_duplicate_firmware_destinations_are_rejected() -> None:
    with pytest.raises(SystemExit, match="firmware destination already exists"):
        create_device_data_initramfs(
            {
                "bluetooth": (_firmware("shared.bin", b"controller"),),
                "fm-radio": (_firmware("shared.bin", b"radio"),),
            }
        )
