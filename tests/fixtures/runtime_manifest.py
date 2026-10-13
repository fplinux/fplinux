# SPDX-License-Identifier: GPL-2.0-only
"""Literal runtime-manifest setup shared by runner and archive consumers."""

from __future__ import annotations

from typing import Any


def runtime_manifest() -> dict[str, Any]:
    """Return one complete runtime contract."""
    image = "image/ramboot.bin"
    adapter = "runner/platform_adapter.py"
    identity_helper = "runner/identity.py"
    ssh_helper = "runner/ssh_transport.py"
    fdl1 = "assets/fdl1.bin"
    loader = "host/loader"
    return {
        "target": "demo",
        "profile": None,
        "build_type": "release",
        "identity": {
            "target": {
                "brand": "Demo",
                "product": "Phone",
                "hardware_codes": ["D-1", "D-2"],
                "compatible": "demo,phone",
                "display_name": "Demo Phone (D-1, D-2)",
            },
            "platform": {
                "name": "ums9117",
                "vendor": "Unisoc",
                "soc": "UMS9117",
                "aliases": ["T117"],
                "compatible": "sprd,ums9117",
                "display_name": "Unisoc UMS9117",
            },
        },
        "transport": "usb-ncm",
        "image": image,
        "personalization": {
            "offset": 1024,
            "bytes": 512,
            "template_sha256": "e" * 64,
        },
        "addresses": {"fdl1": 0x6200, "payload": 0x80100000},
        "usb": {
            "bootrom": {"vendor_id": 0x1782, "product_id": 0x4D00, "wait_seconds": 30},
            "linux_gadget": {
                "vendor_id": 0x0525,
                "product_id": 0xA4A7,
                "wait_seconds": 30,
                "keyboard_interface": 1,
            },
        },
        "assets": {"fdl1": fdl1},
        "adapter": {"kind": "demo"},
        "host_tools": {"loader": loader},
        "sha256": {
            image: "a" * 64,
            adapter: "b" * 64,
            identity_helper: "1" * 64,
            ssh_helper: "c" * 64,
            "runner/loader_events.py": "2" * 64,
            fdl1: "d" * 64,
            loader: "f" * 64,
        },
    }
