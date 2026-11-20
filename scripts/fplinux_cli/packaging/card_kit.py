# SPDX-License-Identifier: GPL-2.0-only
"""Select the verified system-card files included in a release."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any

from fplinux_cli.artifacts.card_kit import CARD_MEMBERS, card_package_members
from fplinux_cli.common import fail
from fplinux_cli.manifests.paths import discover_profiles
from fplinux_cli.packaging.archive import read_bundle_members
from fplinux_cli.runtime.bundle import MICROSD_BOOT_PROFILE

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.artifacts.release_identity import ReleaseContext


def supports_release_card(target: str) -> bool:
    """Select models that have the UMS9117 system-card profile."""
    return MICROSD_BOOT_PROFILE in discover_profiles(target)


def read_card_files(
    bundle: Path, manifest: dict[str, Any], release: ReleaseContext
) -> dict[str, bytes]:
    """Read the card inputs from the same admitted build used for USB boot."""
    records = manifest["files"]
    files = read_bundle_members(bundle, records, [*CARD_MEMBERS, *card_package_members(records)])
    layout = json.loads(files["card/layout.json"])
    if layout["target"] != manifest["target"] or layout["release_id"] != release.release_id:
        fail("microSD kit does not match its release and board")
    return files
