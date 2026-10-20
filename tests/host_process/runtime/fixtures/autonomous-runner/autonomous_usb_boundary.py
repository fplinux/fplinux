# SPDX-License-Identifier: GPL-2.0-only
"""Replace sysfs discovery with case-owned USB and NCM endpoints.

This peer is appended to the delivered adapter and transport modules. Loader,
session creation, authentication decisions and reconnect remain their real code.
"""

import os
from pathlib import Path
from typing import Any

_boundary_root = Path(os.environ["FPLINUX_AUTONOMOUS_TEST_ROOT"]) / "boundary"


def usb_device_path(vendor: int, product: int) -> Path | None:
    """Present a regular local node to the adapter's real access and release checks."""
    del vendor, product
    device = _boundary_root / "bootrom"
    return device if device.exists() else None


def _usb_devices(session: dict[str, Any], *, require_absence_proof: bool = False) -> list[Path]:
    """Supply controlled exact-endpoint presence, including an ambiguous reconnect."""
    del session, require_absence_proof
    device = _boundary_root / "linux-usb"
    if not device.exists():
        return []
    mode = _boundary_root / "reconnect-mode"
    if mode.is_file() and mode.read_text(encoding="ascii") == "ambiguous-endpoint":
        return [device, _boundary_root / "second-linux-usb"]
    return [device]


def _ncm_interface(usb_device: Path, expected_mac: str) -> str:
    """Supply the interface belonging to the controlled USB endpoint."""
    del usb_device, expected_mac
    return "usb0"


def _network_ready(interface: str, session: dict[str, Any]) -> bool:
    """Replace local address and route discovery without configuring a host interface."""
    del interface, session
    return True
