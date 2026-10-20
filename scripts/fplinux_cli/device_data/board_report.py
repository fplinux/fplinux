# SPDX-License-Identifier: GPL-2.0-only
"""Create contributor board reports independently of runtime data preparation."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.common import fail, replace_file_atomically
from fplinux_cli.device_data.prepare import _load_source_module, read_physical_nand

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.device_data.prepare import DeviceDataSpec, DeviceDataTools

BOARD_REPORT_MODULE = "host/stock_image.py"


def write_board_report(
    spec: DeviceDataSpec,
    backup: Path,
    destination: Path,
    tools: DeviceDataTools,
    *,
    extractor: Path,
) -> Path:
    """Read the stock image through its platform extractor and save only its report."""
    if tools.host_tools is None:
        fail("board reports require the prepared platform host tool")
    nand = read_physical_nand(spec, backup, tools)
    provider = _load_source_module(
        extractor, "fplinux_selected_board_report", "board report module"
    )
    if not callable(getattr(provider, "extract_board_report", None)):
        fail("board report module does not expose extract_board_report(nand, host_tools=...)")
    try:
        contents = provider.extract_board_report(nand, host_tools=tools.host_tools)
    except ValueError as error:
        fail(f"board report extraction failed: {error}")
    if not isinstance(contents, bytes):
        fail("board report extractor returned non-byte report contents")
    report = destination / "board-report.json"
    replace_file_atomically(report, contents, 0o600)
    return report
