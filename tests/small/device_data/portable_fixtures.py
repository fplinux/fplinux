# SPDX-License-Identifier: GPL-2.0-only
"""Shared relocated parser and sparse NAND input for portable extraction cases."""

from __future__ import annotations

import shutil
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.device_data.formats import AudioProfileContext
from fplinux_cli.device_data.prepare import DeviceDataSpec, DeviceDataTools, prepare_from_nand

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path


@dataclass(frozen=True)
class SharedExtraction:
    """One relocated selected parser, complete input and caller-selected output path."""

    root: Path
    backup: Path
    spec: DeviceDataSpec
    tools: Path

    @property
    def phone(self) -> Path:
        """Return the same flat phone-store path used by checkout and archive callers."""
        return self.root / "phone"

    def prepare(self) -> Path:
        """Run the canonical shared extractor directly through its maintained interface."""
        return prepare_from_nand(
            self.spec, self.backup, self.phone, DeviceDataTools(host_tools=self.tools)
        )


@pytest.fixture
def shared_extraction(tmp_path: Path) -> SharedExtraction:
    """Own a full sparse physical image and a parser relocated outside the source checkout."""
    parser = tmp_path / "selected-parser.py"
    shutil.copyfile(
        ROOT / "tests/fixtures/portable-archive/portable_device_data_parser.py", parser
    )
    tools = tmp_path / "tools"
    tools.mkdir()
    backup = tmp_path / "nand.bin"
    with backup.open("wb") as stream:
        stream.truncate(138412032)
        stream.write(b"radio")
    spec = DeviceDataSpec(
        target="nokia-ta1618",
        preparation_recipe="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        parser=parser,
        audio=AudioProfileContext(
            machine_compatible=b"nokia,ta1618",
            speaker_vibration=False,
            speaker_from_headphones=False,
            headfree_headphones_only=False,
        ),
        nand={"id": 0x21E5, "raw_page_bytes": 2112},
        groups={
            "bluetooth": [{"source": "radio.bin", "destination": "chip/radio.bin", "size": 5}],
            "audio-profile": [
                {"source": "audio.bin", "destination": "fplinux/audio.bin", "size": 7}
            ],
            "fm-radio": [{"source": "fm.bin", "destination": "chip/fm.bin", "size": 2}],
        },
    )
    return SharedExtraction(tmp_path, backup, spec, tools)
