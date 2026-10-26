# SPDX-License-Identifier: GPL-2.0-only
"""Rendered archive instructions retain exact-phone identity and card safety."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli import common
from fplinux_cli.packaging.readme import append_source_companion, ordinary_readme

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
        "platform": "ums9117",
        "identity": {"brand": "ACME", "product": "Demo", "hardware_codes": ["D-1"]},
        "adapter": {"boot_instructions": "Hold the left soft key while connecting USB."},
        "linux": {"root": {"kind": "initramfs"}},
        "device_data": {
            "parser": "phone_data.py",
            "groups": {"audio-profile": [{"source": "audio.bin", "destination": "audio.bin"}]},
        },
        "audio_policy": {
            "speaker_vibration": False,
            "speaker_from_headphones": False,
            "headfree_headphones_only": False,
        },
        "nand": {"raw_device": "/dev/raw-nand"},
    }


@pytest.mark.parametrize(
    ("target", "platform", "root_kind", "with_groups", "document"),
    [
        pytest.param("demo-phone", "ums9117", "initramfs", True, "ordinary-ram.txt", id="ram"),
        pytest.param(
            "demo-phone", "ums9117", "initramfs", False, "ordinary-headless.txt", id="headless"
        ),
        pytest.param(
            "demo-phone", "ums9117", "external", True, "ordinary-system-card.txt", id="system-card"
        ),
        pytest.param(
            "new-phone", "ums9117", "initramfs", False, "ordinary-bring-up.txt", id="bring-up"
        ),
        pytest.param(
            "demo-phone",
            "sc9820e",
            "initramfs",
            True,
            "ordinary-source-loaders.txt",
            id="source-loaders",
        ),
    ],
)
def test_ordinary_readme_preserves_the_complete_archive_instructions(  # noqa: PLR0913 -- phone, boot, data and document cases remain explicit.
    readme_config: dict[str, Any],
    target: str,
    platform: str,
    root_kind: str,
    *,
    with_groups: bool,
    document: str,
) -> None:
    readme_config["platform"] = platform
    readme_config["linux"]["root"]["kind"] = root_kind
    if platform == "sc9820e":
        readme_config.pop("nand")
    if not with_groups:
        readme_config["device_data"] = {"parser": None, "groups": {}}
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
    assert text.index("Wait until the launcher asks") < text.index("Hold the left soft key")


def test_source_loader_archive_runs_with_its_bundled_stages(
    readme_config: dict[str, Any],
) -> None:
    """A fresh load and reconnect need no separate loader files or import command."""
    readme_config["platform"] = "sc9820e"
    readme_config.pop("nand")
    text = ordinary_readme("demo-phone", readme_config).decode()
    words = " ".join(text.split())

    assert "All files needed to load FPLinux into RAM are included" in words
    assert "does not read or write the phone's internal storage" in words
    load = "python3.14 -I fplinux.py run\n"
    assert load in text
    assert text.index(load) < text.index("Wait until the launcher asks")
    assert "python3.14 -I fplinux.py run --reconnect\n" in text
    assert "--loader-inputs" not in text
    assert "PAC" not in text
    assert "data/session" in text
    assert "--events /path/to/events.jsonl" in text
    assert "complete NAND backup" not in words


def test_emmc_readme_describes_only_selected_originals_and_the_required_restart(
    readme_config: dict[str, Any],
) -> None:
    readme_config["platform"] = "sc9820e"
    readme_config.pop("nand")
    readme_config["emmc"] = {"raw_device": "/dev/mmcblk1"}
    text = ordinary_readme("demo-phone", readme_config).decode()
    words = " ".join(text.split())
    assert "reads only the selected sound settings" in words
    assert 'Keep USB connected until the launcher tells you: "Disconnect USB."' in words
    assert "Start the launcher again with the phone powered off and disconnected" in words
    assert "Your phone's original settings and the files prepared from them" in words
    assert "does not read or write the phone's internal storage" not in words
    assert "backup" not in text


def test_ram_readme_requires_card_release_before_disconnect(readme_config: dict[str, Any]) -> None:
    text = ordinary_readme("demo-phone", readme_config).decode()
    words = " ".join(text.split())

    assert "turn off any swap stored on the card" in words
    assert "Save pending writes and unmount the card's filesystems" in words
    assert "Do not unplug USB or remove the battery until this is done" in words
    assert text.index("Shut down safely") < text.index("After unmounting data cards")
    assert "mount -o remount,ro /" not in text
    assert "FPLINUX.img.xz" not in text
    assert "Start the launcher again with the phone powered off and disconnected" in words
    assert 'Keep USB connected until the launcher tells you: "Disconnect USB."' in words
    assert "data/session" in text
    assert "runtime.log file under logs and shows its full path" in words


def test_system_root_readme_requires_prepared_card_and_observed_read_only_root(
    readme_config: dict[str, Any],
) -> None:
    readme_config["linux"]["root"]["kind"] = "external"
    text = ordinary_readme("demo-phone", readme_config).decode()
    words = " ".join(text.split())

    assert "prepare the microSD system card from FPLINUX.img.xz" in words
    assert "docs/guides/microsd-system-card/index.html" in text
    assert "Insert the prepared card into the powered-off phone" in words
    assert "leave it in until FPLinux has shut down safely" in words
    startup = text.index("Turn the phone off and leave USB disconnected")
    assert text.index("docs/guides/microsd-system-card/index.html") < startup
    assert text.index("Insert the prepared card") < startup
    assert text.index("until FPLinux has shut down safely") < startup
    assert "mount -o remount,ro /" in text
    assert "grep ' / ' /proc/mounts" in text
    assert "If remounting fails, keep the phone powered" in words
    assert text.index("The line for / must show ro") < text.index("After unmounting data cards")
    assert "complete NAND backup" not in words


def test_target_without_a_published_page_keeps_new_phone_archive_instructions(
    readme_config: dict[str, Any],
) -> None:
    text = ordinary_readme("new-phone", readme_config).decode()

    assert "docs/porting/checklist/index.html" in text
    assert "Hold the left soft key while connecting USB." in text
    assert "confirm it on this phone before use" in " ".join(text.split())
