# SPDX-License-Identifier: GPL-2.0-only
"""Build local inputs for the same card actions as the release ZIP."""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING

from fplinux_cli.build.workflow import build
from fplinux_cli.common import ROOT, fail
from fplinux_cli.data_paths import data_root
from fplinux_cli.manifests.paths import discover_profiles
from fplinux_cli.runtime.bundle import MICROSD_BOOT_PROFILE
from fplinux_cli.runtime.bundle_session import resolve_target_bundle
from fplinux_cli.runtime.card import run_card
from fplinux_cli.runtime.card_access import CARD_CID
from fplinux_cli.runtime.cli import confirm_card_erase
from fplinux_cli.runtime.data_paths import checkout_runtime_root

if TYPE_CHECKING:
    from pathlib import Path


def run_target_card(  # noqa: PLR0913 -- build, backup and physical card choices are independent.
    target: str,
    *,
    jobs: int,
    build_type: str,
    erase_card: str | None,
    from_dump: Path | None,
    phone_data: Path | None = None,
    inspect_only: bool = False,
) -> None:
    """Use the normal build commands for development inputs, outside the runtime."""
    if MICROSD_BOOT_PROFILE not in discover_profiles(target):
        fail(f"{target} does not support this microSD install workflow")
    if erase_card is not None and CARD_CID.fullmatch(erase_card) is None:
        fail("--erase-card must be a 32-digit lowercase SD card CID")
    confirm = confirm_card_erase if sys.stdin.isatty() else None
    if not inspect_only and erase_card is None and confirm is None:
        fail("card install requires --erase-card CID")
    build(target, jobs, build_type=build_type, include_device_data=False)
    if not inspect_only:
        build(
            target,
            jobs,
            profile=MICROSD_BOOT_PROFILE,
            build_type=build_type,
            include_device_data=False,
        )
    bundle, _ = resolve_target_bundle(target, build_type=build_type)
    card_bundle_path = None
    if not inspect_only:
        card_bundle, _ = resolve_target_bundle(target, MICROSD_BOOT_PROFILE, build_type=build_type)
        card_bundle_path = card_bundle.path
    cache = data_root(ROOT)
    status = run_card(
        bundle.path,
        checkout_runtime_root(cache, ROOT, target),
        cache / "logs/run" / target,
        card_bundle=card_bundle_path,
        erase_card=erase_card,
        confirm=confirm,
        from_dump=from_dump,
        phone_data=phone_data,
        inspect_only=inspect_only,
    )
    if status:
        raise SystemExit(status)
