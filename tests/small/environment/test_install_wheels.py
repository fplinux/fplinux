# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for exact wheel verification and extraction."""

from __future__ import annotations

import hashlib
import json
import tomllib
import zipfile
from pathlib import Path

import pytest
from fplinux_cli.environment import install_wheels as wheel_installer

_FIXTURES = Path(__file__).parents[2] / "fixtures/environment/wheel"


@pytest.fixture
def wheel_inputs(tmp_path: Path) -> tuple[Path, Path, Path]:
    """Declare one synthetic wheel and two inputs outside the wheel consumer."""
    inputs = tmp_path / "inputs"
    archive = inputs / "downloads/environment/example/example.whl"
    archive.parent.mkdir(parents=True)
    with zipfile.ZipFile(archive, "w") as wheel:
        wheel.writestr(
            "quality_fixture/__init__.py", (_FIXTURES / "__init__.py.fixture").read_bytes()
        )
        wheel.writestr(
            "quality_fixture-1.0.dist-info/METADATA",
            (_FIXTURES / "METADATA").read_bytes(),
        )
    lock = tmp_path / "environment-inputs.json"
    lock.write_text(
        json.dumps(
            tomllib.loads(
                (_FIXTURES / "environment.lock.toml.in.fixture")
                .read_text()
                .format(
                    sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                    size=archive.stat().st_size,
                )
            )
        )
    )
    return lock, inputs, tmp_path / "python"


def test_local_install_extracts_declared_payload(
    wheel_inputs: tuple[Path, Path, Path],
) -> None:
    """A wheel at its declared cache path installs package code and metadata."""
    lock, inputs, destination = wheel_inputs
    wheel_installer.install_wheels(lock, inputs, destination)
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
    document = json.loads(lock.read_text())
    document["input"][0][mismatch] = "0" * 64 if mismatch == "sha256" else 1
    lock.write_text(json.dumps(document))
    with pytest.raises(SystemExit, match=r"wheel (SHA-256|size) mismatch: environment:example"):
        wheel_installer.install_wheels(lock, inputs, destination)
    assert not list(destination.iterdir())


def test_missing_local_wheel_stops_before_extraction(
    wheel_inputs: tuple[Path, Path, Path],
) -> None:
    """A missing declared input is an error rather than a download request."""
    lock, inputs, destination = wheel_inputs
    (inputs / "downloads/environment/example/example.whl").unlink()
    with pytest.raises(FileNotFoundError):
        wheel_installer.install_wheels(lock, inputs, destination)
    assert not list(destination.iterdir())
