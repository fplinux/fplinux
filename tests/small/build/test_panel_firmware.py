# SPDX-License-Identifier: GPL-2.0-only
"""Generated panel firmware selection and synthetic kernel-image verification."""

from __future__ import annotations

import hashlib
import stat
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build.display import render_linux_display_files
from fplinux_cli.build.kernel.configuration import prepare_builtin_firmware
from fplinux_cli.device_data.inputs import FirmwareInput

from tests.fixtures.display import (
    mipi_dbi_display as mipi_dbi_display,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.fixtures.display import (
    mipi_dbi_source as mipi_dbi_source,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.fixtures.display import (
    panel_profile as panel_profile,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.fixtures.display import (
    panel_root as panel_root,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def panel_linux_source(tmp_path: Path, mipi_dbi_display: dict[str, Any]) -> Path:
    """Stage the panel files emitted by the ordinary prepared-Linux producer."""
    source = tmp_path / "linux"
    for relative, contents in render_linux_display_files("demo", mipi_dbi_display).items():
        path = source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
    return source


def test_generated_panel_firmware_is_selected_and_accepted_inside_a_ram_kernel(
    tmp_path: Path, mipi_dbi_display: dict[str, Any], panel_linux_source: Path
) -> None:
    target_config = {"display": mipi_dbi_display, "linux": {"root": {"kind": "initramfs"}}}
    selected = prepare_builtin_firmware(
        target_config, None, source=panel_linux_source, work=tmp_path / "work"
    )
    arguments = selected.kconfig_arguments()
    assert arguments == [
        "--set-str",
        "EXTRA_FIRMWARE",
        "demo-panel.bin",
        "--set-str",
        "EXTRA_FIRMWARE_DIR",
        str(tmp_path / "work/builtin-firmware"),
    ]
    expected = b"MIPI DBI\0\0\0\0\0\0\0\1\x11\0\0\1\x78\x3a\1\x55\x29\0"
    assert (panel_linux_source / "firmware/demo-panel.bin").read_bytes() == expected
    assert (tmp_path / "work/builtin-firmware/demo-panel.bin").read_bytes() == expected
    config = tmp_path / ".config"
    config.write_text(
        'CONFIG_EXTRA_FIRMWARE="demo-panel.bin"\n'
        f'CONFIG_EXTRA_FIRMWARE_DIR="{tmp_path / "work/builtin-firmware"}"\n',
        encoding="ascii",
    )
    vmlinux = tmp_path / "vmlinux"
    vmlinux.write_bytes(b"kernel prefix" + expected + b"kernel suffix")

    selected.verify(config, vmlinux)


@pytest.mark.parametrize(
    ("firmware", "directory", "embedded", "diagnostic"),
    [
        ("other.bin", "builtin-firmware", True, "configuration lost"),
        ("demo-panel.bin", "other", True, "configuration lost"),
        ("demo-panel.bin", "builtin-firmware", False, "does not embed"),
    ],
)
def test_panel_firmware_verifier_rejects_missing_selection_or_bytes(  # noqa: PLR0913 -- configuration and artifact expectations remain explicit.
    tmp_path: Path,
    mipi_dbi_display: dict[str, Any],
    panel_linux_source: Path,
    firmware: str,
    directory: str,
    *,
    embedded: bool,
    diagnostic: str,
) -> None:
    selected = prepare_builtin_firmware(
        {"display": mipi_dbi_display, "linux": {"root": {"kind": "initramfs"}}},
        None,
        source=panel_linux_source,
        work=tmp_path,
    )
    config = tmp_path / ".config"
    config.write_text(
        f'CONFIG_EXTRA_FIRMWARE="{firmware}"\n'
        f'CONFIG_EXTRA_FIRMWARE_DIR="{tmp_path / directory}"\n',
        encoding="ascii",
    )
    vmlinux = tmp_path / "vmlinux"
    vmlinux.write_bytes(
        b"prefix"
        + (b"MIPI DBI\0\0\0\0\0\0\0\1\x11\0\0\1\x78\x3a\1\x55\x29\0" if embedded else b"")
        + b"suffix"
    )
    with pytest.raises(SystemExit, match=diagnostic):
        selected.verify(config, vmlinux)


def test_ram_kernel_rejects_additional_firmware_beside_the_generated_panel(
    tmp_path: Path, mipi_dbi_display: dict[str, Any], panel_linux_source: Path
) -> None:
    config = tmp_path / ".config"
    config.write_text(
        'CONFIG_EXTRA_FIRMWARE="demo-panel.bin phone-profile.bin"\n'
        f'CONFIG_EXTRA_FIRMWARE_DIR="{tmp_path / "firmware"}"\n',
        encoding="ascii",
    )
    selected = prepare_builtin_firmware(
        {"display": mipi_dbi_display, "linux": {"root": {"kind": "initramfs"}}},
        None,
        source=panel_linux_source,
        work=tmp_path,
    )
    with pytest.raises(SystemExit, match="configuration lost"):
        selected.verify(config, tmp_path / "unused-vmlinux")


def test_ums9117_panel_does_not_add_builtin_firmware(
    tmp_path: Path, panel_display: dict[str, Any]
) -> None:
    selected = prepare_builtin_firmware(
        {"display": panel_display, "linux": {"root": {"kind": "initramfs"}}},
        None,
        source=tmp_path,
        work=tmp_path,
    )
    assert selected.files == ()
    config = tmp_path / ".config"
    config.write_text('CONFIG_EXTRA_FIRMWARE=""\n', encoding="ascii")
    selected.verify(config, tmp_path / "unused-vmlinux")


@pytest.mark.parametrize(
    ("root_kind", "with_panel", "names", "modes"),
    [
        ("external", False, "phone-profile.bin", (0o600,)),
        ("initramfs", True, "demo-panel.bin", (0o644,)),
        ("external", True, "demo-panel.bin phone-profile.bin", (0o644, 0o600)),
    ],
    ids=["ums-audio", "qin-ram-panel", "qin-external-panel-and-audio"],
)
def test_builtin_firmware_composes_the_selected_boot_consumers(  # noqa: PLR0913
    tmp_path: Path,
    mipi_dbi_display: dict[str, Any],
    panel_linux_source: Path,
    root_kind: str,
    *,
    with_panel: bool,
    names: str,
    modes: tuple[int, ...],
) -> None:
    """Panel and fitted audio share one Kconfig directory while preserving private file modes."""
    profile = b"literal phone sound settings\0\xff"
    audio = (
        FirmwareInput(
            "audio.bin", "phone-profile.bin", profile, hashlib.sha256(profile).hexdigest()
        ),
    )
    display = mipi_dbi_display if with_panel else {"panel": None}
    selected = prepare_builtin_firmware(
        {"display": display, "linux": {"root": {"kind": root_kind}}},
        audio,
        source=panel_linux_source,
        work=tmp_path,
    )
    arguments = selected.kconfig_arguments()
    assert arguments[2] == names
    assert arguments[5] == str(tmp_path / "builtin-firmware")
    assert stat.S_IMODE((tmp_path / "builtin-firmware").stat().st_mode) == 0o700
    assert (
        tuple(
            stat.S_IMODE((tmp_path / "builtin-firmware" / name).stat().st_mode)
            for name in names.split()
        )
        == modes
    )
    panel_bytes = b"MIPI DBI\0\0\0\0\0\0\0\1\x11\0\0\1\x78\x3a\1\x55\x29\0"
    if with_panel:
        assert (tmp_path / "builtin-firmware/demo-panel.bin").read_bytes() == panel_bytes
    if root_kind == "external":
        assert (tmp_path / "builtin-firmware/phone-profile.bin").read_bytes() == profile
    else:
        assert not (tmp_path / "builtin-firmware/phone-profile.bin").exists()
    config = tmp_path / ".config"
    config.write_text(
        f'CONFIG_EXTRA_FIRMWARE="{names}"\n'
        f'CONFIG_EXTRA_FIRMWARE_DIR="{tmp_path / "builtin-firmware"}"\n',
        encoding="ascii",
    )
    vmlinux = tmp_path / "vmlinux"
    vmlinux.write_bytes(
        b"kernel"
        + (panel_bytes if with_panel else b"")
        + (profile if root_kind == "external" else b"")
    )
    selected.verify(config, vmlinux)
