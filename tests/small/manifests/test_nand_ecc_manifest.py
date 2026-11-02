# SPDX-License-Identifier: GPL-2.0-only
"""Admit explicit NAND correction formats at the target manifest boundary."""

from __future__ import annotations

from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.manifests import targets


def test_declared_bch12_format_preserves_the_chip_expectation() -> None:
    """A selected correction format retains the identity used for backup admission."""
    raw = common.load_toml(common.ROOT / "targets/nokia-ta1618/target.toml")
    declaration = {
        "raw_device": "/dev/ums9117-nand-raw",
        "id": 0x81C8,
        "raw_page_bytes": 2176,
        "ecc_format": "ums9117-bch12",
    }
    raw["nand"] = declaration
    with mock.patch.object(targets, "load_toml", return_value=raw):
        config = targets.load_target("nokia-ta1618")
    assert config["nand"] == declaration


@pytest.mark.parametrize(
    ("fields", "message"),
    [
        (
            {"ecc_format": "ums9117-bch12"},
            "ecc_format also requires the declared chip",
        ),
        (
            {"id": 0x81C8, "raw_page_bytes": 2112, "ecc_format": "ums9117-bch12"},
            "2176-byte UMS9117 pages",
        ),
        (
            {"id": 0x81C8, "raw_page_bytes": 2176, "ecc_format": "unknown"},
            "ECC format must be ums9117-bch12",
        ),
    ],
    ids=["missing-chip", "other-geometry", "unknown-format"],
)
def test_unsupported_nand_correction_is_rejected(fields: dict[str, object], message: str) -> None:
    """Unknown formats and incomplete geometry cannot select a correction path."""
    raw = common.load_toml(common.ROOT / "targets/nokia-ta1618/target.toml")
    raw["nand"] = {"raw_device": "/dev/ums9117-nand-raw", **fields}
    with (
        mock.patch.object(targets, "load_toml", return_value=raw),
        pytest.raises(SystemExit, match=message),
    ):
        targets.load_target("nokia-ta1618")
