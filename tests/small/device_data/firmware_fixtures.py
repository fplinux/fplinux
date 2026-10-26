# SPDX-License-Identifier: GPL-2.0-only
"""Complete synthetic Qin radio stores published through the preparation API."""

from __future__ import annotations

import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest
from fplinux_cli.device_data.formats import PreparedGroup
from fplinux_cli.device_data.prepare import (
    materialize_device_data,
    publish_phone_data,
)


@dataclass(frozen=True)
class QinFirmwareContext:
    """Own a target's source declarations and isolated private phone store."""

    root: Path
    cache: Path
    target: str
    groups: dict[str, list[dict[str, Any]]]

    def publish(
        self,
        *,
        system_calibration: bytes = b"system calibration A\x00",
        report: bytes = b"report A\n",
    ) -> Path:
        """Replace prepared firmware and reports without persisting duplicate originals."""
        prepared = {
            "wcnmodem.bin": b"radio payload\x00\x01",
            "system-config.bin": b"system configuration\x00",
            "system-cali.bin": system_calibration,
            "wcn-config.bin": b"wcn configuration\x00",
            "wcn-cali.bin": b"wcn calibration\x00",
        }
        group = PreparedGroup(
            originals={"original.bin": b"synthetic original\x00"},
            prepared=prepared,
            reports={"summary.txt": report},
        )
        produced = {"wcn-firmware": group}
        data = self.root / "data/fplinux/phones" / self.target
        data.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".phone-", dir=data))
        materialize_device_data(staging, produced, self.groups)
        return publish_phone_data(staging, data / "phone", replace=True)


@pytest.fixture
def qin_firmware(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> QinFirmwareContext:
    """Own complete synthetic input bytes; no phone data or ambient cache is read."""
    cache = tmp_path / ".cache"
    cache.mkdir()
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "data"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "local"))
    groups: dict[str, list[dict[str, Any]]] = {
        "wcn-firmware": [
            {"source": "wcnmodem.bin", "destination": "wcnmodem.bin", "size": 15},
            {
                "source": "system-config.bin",
                "destination": "qin1splus/system-config.bin",
                "size": 21,
            },
            {
                "source": "system-cali.bin",
                "destination": "qin1splus/system-cali.bin",
                "size": 21,
            },
            {
                "source": "wcn-config.bin",
                "destination": "qin1splus/wcn-config.bin",
                "size": 18,
            },
            {
                "source": "wcn-cali.bin",
                "destination": "qin1splus/wcn-cali.bin",
                "size": 16,
            },
        ]
    }
    context = QinFirmwareContext(tmp_path, cache, "xiaomi-qin-1s-plus", groups)
    context.publish()
    return context
