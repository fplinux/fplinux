# SPDX-License-Identifier: GPL-2.0-only
"""Build local inputs for the same card actions as the release ZIP."""

from __future__ import annotations

import sys
from functools import partial
from typing import TYPE_CHECKING

from fplinux_cli.build.workflow import build
from fplinux_cli.common import ROOT, fail
from fplinux_cli.data_paths import data_root
from fplinux_cli.manifests.paths import discover_profiles
from fplinux_cli.runtime.bundle import MICROSD_BOOT_PROFILE
from fplinux_cli.runtime.bundle_session import resolve_target_bundle
from fplinux_cli.runtime.cli import confirm_card_action
from fplinux_cli.runtime.data_paths import checkout_runtime_root
from fplinux_cli.runtime.operations import BundlePaths, card_selection_error, execute_operation

if TYPE_CHECKING:
    from fplinux_cli.runtime.operations import CardRequest


def run_target_card(
    request: CardRequest,
    *,
    jobs: int,
    build_type: str,
) -> None:
    """Use the normal build commands for development inputs, outside the runtime."""
    target = request.target
    if target is None:
        fail("Select a phone model")
    if MICROSD_BOOT_PROFILE not in discover_profiles(target):
        fail(f"{target} does not support this microSD workflow")
    confirm = partial(confirm_card_action, action=request.action) if sys.stdin.isatty() else None
    error = card_selection_error(
        request.action,
        request.erase_card,
        request.card_cid,
        request.build_id,
        may_confirm=confirm is not None,
    )
    if error is not None:
        fail(error)
    build(target, jobs, build_type=build_type, include_device_data=False)
    if request.action != "inspect":
        build(
            target,
            jobs,
            profile=MICROSD_BOOT_PROFILE,
            build_type=build_type,
            include_device_data=False,
        )
    bundle, _ = resolve_target_bundle(target, build_type=build_type)
    card_bundle_path = None
    if request.action != "inspect":
        card_bundle, _ = resolve_target_bundle(target, MICROSD_BOOT_PROFILE, build_type=build_type)
        card_bundle_path = card_bundle.path
    cache = data_root(ROOT)
    status = execute_operation(
        request,
        BundlePaths(
            bundle.path,
            checkout_runtime_root(cache, ROOT, target),
            cache / "logs/run" / target,
            card_bundle=card_bundle_path,
        ),
        confirm_card=confirm,
    )
    if status:
        raise SystemExit(status)
