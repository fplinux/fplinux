# SPDX-License-Identifier: GPL-2.0-only
"""Board RAM limits override the platform handoff without changing other boards."""

from __future__ import annotations

from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build.bootstrap.recipe import effective_boot_layout
from fplinux_cli.manifests import targets


def test_smaller_board_selects_its_own_ram_and_handoff_arenas() -> None:
    """A 48 MiB board never inherits payload destinations beyond its RAM."""
    raw = common.load_toml(common.ROOT / "targets/nokia-ta1618/target.toml")
    layout = {
        "ram_base": 0x80000000,
        "ram_size": 0x03000000,
        "kernel_load": 0x82000000,
        "kernel_entry": 0x82000000,
        "kernel_size": 0x00C00000,
        "fdt_load": 0x82E00000,
        "fdt_size": 0x00010000,
        "framebuffer": 0x82F00000,
        "framebuffer_size": 0x00100000,
        "timer_hz": 1000,
    }
    raw["bootstrap"]["layout"] = layout
    raw["linux"]["memory"]["size"] = 0x02C00000
    with mock.patch.object(targets, "load_toml", return_value=raw):
        selected = targets.load_target("nokia-ta1618")
    assert selected["bootstrap"]["layout"] == layout
    assert effective_boot_layout(selected) == layout
    assert targets.load_target("nokia-ta1618")["bootstrap"]["layout"]["ram_size"] == 0x04000000


def test_linux_memory_must_fit_the_board_ram_limit() -> None:
    """A board override cannot leave Linux using the platform's larger RAM range."""
    raw = common.load_toml(common.ROOT / "targets/konrow-senior-280-plus/target.toml")
    raw["linux"]["memory"]["size"] = 0x03C00000
    with (
        mock.patch.object(targets, "load_toml", return_value=raw),
        pytest.raises(SystemExit, match="Linux memory must fit inside physical RAM"),
    ):
        targets.load_target("konrow-senior-280-plus")
