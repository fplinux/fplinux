# SPDX-License-Identifier: GPL-2.0-only
"""Rendered archive instructions retain exact-phone identity and card safety."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli import common
from fplinux_cli.cli.package_readme import append_source_companion, ordinary_readme

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def readme_config(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> dict[str, Any]:
    routes = tmp_path / "site/src/data/phones.json"
    routes.parent.mkdir(parents=True)
    routes.write_text(json.dumps([{"id": "demo-phone", "slug": "phones/demo-model"}]))
    monkeypatch.setattr(common, "ROOT", tmp_path)
    return {
        "identity": {"brand": "ACME", "product": "Demo", "hardware_codes": ["D-1"]},
        "adapter": {"boot_instructions": "Hold the left soft key while connecting USB."},
        "linux": {"root": {"kind": "initramfs"}},
    }


@pytest.mark.parametrize(
    ("target", "root_kind", "document"),
    [
        pytest.param("demo-phone", "initramfs", "ordinary-ram.txt", id="ram"),
        pytest.param("demo-phone", "external", "ordinary-system-card.txt", id="system-card"),
        pytest.param("new-phone", "initramfs", "ordinary-bring-up.txt", id="bring-up"),
    ],
)
def test_ordinary_readme_preserves_the_complete_archive_instructions(
    readme_config: dict[str, Any], target: str, root_kind: str, document: str
) -> None:
    readme_config["linux"]["root"]["kind"] = root_kind
    expected = ROOT / "tests/small/cli/fixtures/package-readme" / document

    assert ordinary_readme(target, readme_config) == expected.read_bytes()


def test_ordinary_readme_preserves_literal_and_multiline_boot_instructions(
    readme_config: dict[str, Any],
) -> None:
    readme_config["adapter"]["boot_instructions"] = (
        'Hold {boot} @NAME@ with $HOME, "USB", \\ and\nkeep it held.'
    )

    text = ordinary_readme("demo-phone", readme_config)

    assert b'  Hold {boot} @NAME@ with $HOME, "USB", \\ and\nkeep it held.\n' in text


def test_source_companion_footer_preserves_the_literal_archive_name() -> None:
    text = append_source_companion(b"Archive instructions\n", "sources-{build}@$HOME")

    assert text == (
        b"Archive instructions\n\n"
        b"Matching source and build material:\n"
        b"  sources-{build}@$HOME.zip\n"
        b"Keep it available alongside this archive. Licenses remain included here.\n"
    )


def test_readme_names_the_selected_variant_and_existing_offline_route(
    readme_config: dict[str, Any],
) -> None:
    text = ordinary_readme("demo-phone", readme_config).decode()

    assert text.startswith("FPLinux for ACME Demo (D-1)\n")
    assert "Hold the left soft key while connecting USB." in text
    assert "docs/phones/demo-model/index.html" in text
    assert text.index("Wait until the runner asks") < text.index("Hold the left soft key")
    assert "XDG_RUNTIME_DIR must name an existing absolute directory owned by your user" in text
    assert "a normal Linux login session provides it" in text


def test_ram_readme_requires_card_release_before_disconnect(readme_config: dict[str, Any]) -> None:
    text = ordinary_readme("demo-phone", readme_config).decode()
    words = " ".join(text.split())

    assert "disable card-backed swap, flush writes and unmount" in words
    assert "Do not unplug USB or remove the battery while a writable card filesystem" in words
    assert text.index("unmount all data-card") < text.index("Only after releasing the cards")
    assert "mount -o remount,ro /" not in text
    assert "FPLINUX.img.xz" not in text


def test_system_root_readme_requires_prepared_card_and_observed_read_only_root(
    readme_config: dict[str, Any],
) -> None:
    readme_config["linux"]["root"]["kind"] = "external"
    text = ordinary_readme("demo-phone", readme_config).decode()
    words = " ".join(text.split())

    assert "prepare the microSD system card from FPLINUX.img.xz" in words
    assert "docs/guides/microsd-system-card/index.html" in text
    assert "Insert the prepared card into the powered-off phone" in words
    assert "keep it installed for the whole powered session" in words
    startup = text.index("Keep the powered-off phone disconnected")
    assert text.index("docs/guides/microsd-system-card/index.html") < startup
    assert text.index("Insert the prepared card") < startup
    assert text.index("for the whole powered session") < startup
    assert "mount -o remount,ro /" in text
    assert "grep ' / ' /proc/mounts" in text
    assert "If remounting fails, keep the phone powered" in text
    assert text.index("The root entry must show ro") < text.index("Only after releasing the cards")


def test_target_without_a_published_page_keeps_new_phone_archive_instructions(
    readme_config: dict[str, Any],
) -> None:
    text = ordinary_readme("new-phone", readme_config).decode()

    assert "docs/porting/checklist/index.html" in text
    assert "Hold the left soft key while connecting USB." in text
    assert "confirm it on this phone before use" in " ".join(text.split())
