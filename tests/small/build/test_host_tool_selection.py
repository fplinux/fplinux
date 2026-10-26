# SPDX-License-Identifier: GPL-2.0-only
"""Native host selection with controlled compilation and ELF inspection boundaries."""

from __future__ import annotations

import struct
import subprocess
from typing import TYPE_CHECKING

import pytest
from fplinux_cli import common
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build.host_tools import host
from fplinux_cli.build.runtime import (
    device_data_descriptor,
    runtime_host_tools,
    runtime_needs_nand_ecc,
)
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path
    from typing import Any


@pytest.fixture
def native_host_compiler(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Replace compilation and readelf while retaining recipe selection and publication."""
    monkeypatch.setattr(inputs_build, "CACHE", tmp_path / "cache")
    monkeypatch.setenv("FPLINUX_ENVIRONMENT_RECIPE", "1" * 64)
    monkeypatch.setenv("FPLINUX_ENVIRONMENT_CONTENT", "2" * 64)

    def binary(recipe: dict[str, Any], output: Path) -> Path:
        contents = bytearray(64)
        contents[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", contents, 18, 62)
        struct.pack_into("<H", contents, 54, 56)
        destination = output / recipe["name"]
        destination.write_bytes(contents + recipe["name"].encode("ascii"))
        destination.chmod(0o755)
        return destination

    def make_binary(
        _sources: dict[str, Any], recipe: dict[str, Any], _work: Path, output: Path
    ) -> Path:
        return binary(recipe, output)

    def inspect(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        assert command[:2] == ["readelf", "-dW"]
        return subprocess.CompletedProcess(command, 0, "No dynamic section\n", "")

    monkeypatch.setattr(host, "build_make_host_tool", make_binary)
    monkeypatch.setattr(host, "build_cc_libusb_tool", binary)
    monkeypatch.setattr(host.subprocess, "run", inspect)


@pytest.mark.parametrize(
    ("target", "ecc_module", "expected"),
    [
        (
            "nokia-ta1618",
            None,
            {"spd_dump", "libc_server", "fplinux-usb-keyboard"},
        ),
        (
            "nokia-110-4g",
            None,
            {"spd_dump", "libc_server", "fplinux-usb-keyboard"},
        ),
        (
            "bq-2830l-tank-connect-max",
            "platforms/ums9117/host/nand_ecc.py",
            {"spd_dump", "libc_server", "fplinux-usb-keyboard", "ums9117-nand-ecc"},
        ),
    ],
)
def test_target_host_build_publishes_only_runtime_tools_and_selected_ecc(
    target: str,
    ecc_module: str | None,
    expected: set[str],
    tmp_path: Path,
    native_host_compiler: None,
) -> None:
    """Ordinary RAM bundles omit report tools and preserve the real NAND decoder consumer."""
    config = load_target(target)
    platform = load_platform(config["platform"])
    descriptor = device_data_descriptor(target, config)
    assert descriptor is not None
    assert descriptor["ecc_module"] == ecc_module
    selected = runtime_host_tools(platform, nand_ecc=runtime_needs_nand_ecc(config))

    files = host.build_host_tools(
        common.load_toml(ROOT / "sources.lock.toml"),
        platform,
        tmp_path / "build",
        tools=selected.values(),
    )

    assert set(files) == expected
    assert {path.name for path in (tmp_path / "build/host").iterdir()} == expected
    for name, path in files.items():
        assert path.read_bytes()[64:] == name.encode("ascii")
        assert path.stat().st_mode & 0o777 == 0o755


def test_explicit_board_report_host_build_publishes_the_report_helper(
    tmp_path: Path, native_host_compiler: None
) -> None:
    """Contributor reports retain their declared tool without adding ordinary runtime files."""
    files = host.build_host_tools(
        common.load_toml(ROOT / "sources.lock.toml"),
        load_platform("ums9117"),
        tmp_path / "build",
        tools=("fphelper_t117",),
    )

    assert set(files) == {"fphelper_t117"}
    assert files["fphelper_t117"].read_bytes()[64:] == b"fphelper_t117"


def test_missing_requested_host_recipe_fails_before_publishing_any_binary(tmp_path: Path) -> None:
    """A selected runtime dependency cannot disappear silently from the build result."""
    with pytest.raises(SystemExit, match="host tools are not declared: missing"):
        host.build_host_tools(
            {},
            {"host": {"tools": []}},
            tmp_path / "build",
            tools=("missing",),
        )

    assert not (tmp_path / "build").exists()
