# SPDX-License-Identifier: GPL-2.0-only
"""Generated panel firmware selection and synthetic kernel-image verification."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build.display import render_linux_display_files
from fplinux_cli.build.kernel.configuration import (
    assert_external_firmware_kconfig,
    panel_firmware_kconfig_arguments,
    verify_builtin_panel_firmware,
)

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


def test_generated_panel_firmware_is_selected_and_accepted_inside_a_ram_kernel(
    tmp_path: Path, mipi_dbi_display: dict[str, Any]
) -> None:
    source = tmp_path / "linux"
    target_config = {"display": mipi_dbi_display}
    for relative, contents in render_linux_display_files("demo", mipi_dbi_display).items():
        path = source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
    arguments = panel_firmware_kconfig_arguments(target_config, source)
    assert arguments == [
        "--set-str",
        "EXTRA_FIRMWARE",
        "demo-panel.bin",
        "--set-str",
        "EXTRA_FIRMWARE_DIR",
        str(source / "firmware"),
    ]
    expected = b"MIPI DBI\0\0\0\0\0\0\0\1\x11\0\0\1\x78\x3a\1\x55\x29\0"
    assert (source / "firmware/demo-panel.bin").read_bytes() == expected
    config = tmp_path / ".config"
    config.write_text(
        'CONFIG_EXTRA_FIRMWARE="demo-panel.bin"\n'
        f'CONFIG_EXTRA_FIRMWARE_DIR="{source / "firmware"}"\n',
        encoding="ascii",
    )
    vmlinux = tmp_path / "vmlinux"
    vmlinux.write_bytes(b"kernel prefix" + expected + b"kernel suffix")

    assert_external_firmware_kconfig(config, target_config, source)
    verify_builtin_panel_firmware(target_config, config, vmlinux, source)


@pytest.mark.parametrize(
    ("firmware", "directory", "embedded", "diagnostic"),
    [
        ("other.bin", "firmware", True, "configuration lost"),
        ("demo-panel.bin", "other", True, "configuration lost"),
        ("demo-panel.bin", "firmware", False, "does not embed"),
    ],
)
def test_panel_firmware_verifier_rejects_missing_selection_or_bytes(  # noqa: PLR0913 -- configuration and artifact expectations remain explicit.
    tmp_path: Path,
    mipi_dbi_display: dict[str, Any],
    firmware: str,
    directory: str,
    *,
    embedded: bool,
    diagnostic: str,
) -> None:
    source = tmp_path / "linux"
    config = tmp_path / ".config"
    config.write_text(
        f'CONFIG_EXTRA_FIRMWARE="{firmware}"\nCONFIG_EXTRA_FIRMWARE_DIR="{source / directory}"\n',
        encoding="ascii",
    )
    vmlinux = tmp_path / "vmlinux"
    vmlinux.write_bytes(
        b"prefix"
        + (b"MIPI DBI\0\0\0\0\0\0\0\1\x11\0\0\1\x78\x3a\1\x55\x29\0" if embedded else b"")
        + b"suffix"
    )
    with pytest.raises(SystemExit, match=diagnostic):
        verify_builtin_panel_firmware({"display": mipi_dbi_display}, config, vmlinux, source)


def test_ram_kernel_rejects_additional_firmware_beside_the_generated_panel(
    tmp_path: Path, mipi_dbi_display: dict[str, Any]
) -> None:
    config = tmp_path / ".config"
    config.write_text(
        'CONFIG_EXTRA_FIRMWARE="demo-panel.bin phone-profile.bin"\n'
        f'CONFIG_EXTRA_FIRMWARE_DIR="{tmp_path / "firmware"}"\n',
        encoding="ascii",
    )
    with pytest.raises(SystemExit, match="RAM kernel unexpectedly contains built-in firmware"):
        assert_external_firmware_kconfig(config, {"display": mipi_dbi_display}, tmp_path)


def test_ums9117_panel_does_not_add_builtin_firmware(
    tmp_path: Path, panel_display: dict[str, Any]
) -> None:
    target_config = {"display": panel_display}
    assert panel_firmware_kconfig_arguments(target_config, tmp_path) == []
    verify_builtin_panel_firmware(
        target_config, tmp_path / "unused-config", tmp_path / "unused-vmlinux", tmp_path
    )
