# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for exact wheel verification and extraction."""

from __future__ import annotations

import hashlib
import subprocess
import zipfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.environment import install_wheels as wheel_installer

if TYPE_CHECKING:
    from collections.abc import Sequence


@pytest.fixture
def wheel_inputs(tmp_path: Path) -> tuple[Path, Path, Path]:
    """Declare one synthetic wheel and two inputs outside the wheel consumer."""
    inputs = tmp_path / "inputs"
    archive = inputs / "sources/example/example.whl"
    archive.parent.mkdir(parents=True)
    with zipfile.ZipFile(archive, "w") as wheel:
        wheel.writestr("quality_fixture/__init__.py", b"VALUE = 'declared'\n")
        wheel.writestr(
            "quality_fixture-1.0.dist-info/METADATA",
            b"Metadata-Version: 2.1\nName: quality-fixture\nVersion: 1.0\n",
        )
    lock = tmp_path / "environment.lock.toml"
    lock.write_text(
        '[[input]]\nkey = "container:example"\n'
        'url = "https://example.invalid/example.whl"\n'
        f'sha256 = "{hashlib.sha256(archive.read_bytes()).hexdigest()}"\n'
        f"bytes = {archive.stat().st_size}\n"
        'destination = "downloads/environment/example/example.whl"\n'
        'purpose = "container-source"\narch = "x86_64"\n'
        '[[input]]\ndestination = "downloads/environment/package/unused.whl"\n'
        'purpose = "container-package"\n'
        '[[input]]\ndestination = "downloads/environment/source/unused.tar.gz"\n'
        'purpose = "container-source"\n'
    )
    return lock, inputs, tmp_path / "python"


def test_offline_install_extracts_declared_payload_without_network(
    wheel_inputs: tuple[Path, Path, Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    """A saved wheel installs package code and metadata without a download."""

    def reject_network(*_args: object, **_kwargs: object) -> None:
        pytest.fail("offline wheel installation attempted a network subprocess")

    monkeypatch.setattr(subprocess, "run", reject_network)
    lock, inputs, destination = wheel_inputs
    wheel_installer.install_wheels(lock, inputs, destination, offline=True)
    assert (destination / "quality_fixture/__init__.py").read_bytes() == b"VALUE = 'declared'\n"
    assert (destination / "quality_fixture-1.0.dist-info/METADATA").read_bytes() == (
        b"Metadata-Version: 2.1\nName: quality-fixture\nVersion: 1.0\n"
    )


@pytest.mark.parametrize("mismatch", ["sha256", "bytes"])
def test_mismatched_wheel_is_rejected_before_extraction(
    wheel_inputs: tuple[Path, Path, Path], mismatch: str
) -> None:
    """An archive that does not meet its declaration cannot install payload."""
    lock, inputs, destination = wheel_inputs
    lines = lock.read_text().splitlines()
    replacement = f'sha256 = "{"0" * 64}"' if mismatch == "sha256" else "bytes = 1"
    lock.write_text(
        "\n".join(replacement if line.startswith(f"{mismatch} = ") else line for line in lines)
        + "\n"
    )
    with pytest.raises(SystemExit, match=r"wheel (SHA-256|size) mismatch: container:example"):
        wheel_installer.install_wheels(lock, inputs, destination, offline=True)
    assert not list(destination.iterdir())


def test_online_download_keeps_retry_policy_and_extracts_verified_payload(
    wheel_inputs: tuple[Path, Path, Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    """A controlled curl boundary supplies the exact archive before extraction."""
    lock, inputs, destination = wheel_inputs
    contents = (inputs / "sources/example/example.whl").read_bytes()
    downloads: list[list[str]] = []

    def curl_boundary(command: Sequence[str], *, check: bool) -> None:
        downloads.append(list(command))
        assert check
        Path(command[5]).write_bytes(contents)

    monkeypatch.setattr(subprocess, "run", curl_boundary)
    wheel_installer.install_wheels(lock, inputs, destination, offline=False)
    assert len(downloads) == 1
    assert downloads[0][:5] == ["curl", "-fsSL", "--retry", "3", "--output"]
    assert downloads[0][-1] == "https://example.invalid/example.whl"
    assert (destination / "quality_fixture/__init__.py").read_bytes() == b"VALUE = 'declared'\n"


def test_online_download_failure_stops_before_extraction(
    wheel_inputs: tuple[Path, Path, Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    """A failed external download preserves its failure and installs no payload."""
    lock, inputs, destination = wheel_inputs

    def failed_download(command: Sequence[str], *, check: bool) -> None:
        assert check
        raise subprocess.CalledProcessError(22, command)

    monkeypatch.setattr(subprocess, "run", failed_download)
    with pytest.raises(subprocess.CalledProcessError) as failure:
        wheel_installer.install_wheels(lock, inputs, destination, offline=False)
    assert failure.value.returncode == 22
    assert not list(destination.iterdir())
