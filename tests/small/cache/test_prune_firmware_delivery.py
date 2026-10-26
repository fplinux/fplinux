# SPDX-License-Identifier: GPL-2.0-only
"""Prune retains current Qin rootfs variants rather than obsolete inputs or report bytes."""

from __future__ import annotations

from functools import partial
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli.alpine import recipes, rootfs_state, signing
from fplinux_cli.artifacts.release import ReleaseContext
from fplinux_cli.cache.prune import alpine as prune_alpine
from fplinux_cli.cache.prune.operations import discard_obsolete_rootfs
from fplinux_cli.device_data.inputs import capture_external_device_data
from fplinux_cli.environment.definition import environment_artifact_recipe_digest
from fplinux_cli.environment.state import EnvironmentState, publish_environment_state

from tests.small.alpine.fixtures import (
    alpine_source as alpine_source,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.small.device_data.firmware_fixtures import (
    qin_firmware as qin_firmware,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.small.alpine.fixtures import AlpineSourceContext
    from tests.small.device_data.firmware_fixtures import QinFirmwareContext


def test_prune_retains_qin_system_root_variants_and_expires_changed_inputs(
    qin_firmware: QinFirmwareContext, alpine_source: AlpineSourceContext
) -> None:
    """System roots with and without private firmware remain available until inputs change."""
    root_kind = "external"
    cache = qin_firmware.cache
    public_key = signing.signing_public_key(cache)
    public_key.parent.mkdir(parents=True)
    public_key.write_bytes(b"public test key\n")
    publish_environment_state(cache, EnvironmentState("a" * 64, "b" * 64, "b" * 64))
    environment_recipe = environment_artifact_recipe_digest("a" * 64, "b" * 64)
    signing_key = signing.signing_key_identity(cache)
    release_context = ReleaseContext("012345abcdef", "012345abcdef" + "a" * 28, "b" * 64)
    rootfs_recipe = partial(
        recipes.alpine_rootfs_recipe,
        root=alpine_source.root,
        release_id=release_context.release_id,
    )

    def selected_recipe() -> str:
        selected = capture_external_device_data(qin_firmware.target, qin_firmware.groups)
        return rootfs_recipe(
            environment_recipe,
            signing_key,
            (),
            firmware_inputs=selected["wcn-firmware"],
            root_kind=root_kind,
        )

    first = selected_recipe()
    bare = rootfs_recipe(environment_recipe, signing_key, (), root_kind=root_kind)
    assert first != bare
    for recipe in (first, bare):
        output = rootfs_state.rootfs_output(cache, recipe)
        output.mkdir(parents=True)
        (output / "rootfs.cpio").write_bytes(b"synthetic rootfs artifact\n")
        rootfs_state.write_receipt(output, recipe)

    target = {
        "platform": "sc9820e",
        "linux": {"root": {"kind": root_kind}},
        "device_data": {"groups": qin_firmware.groups},
    }
    with (
        mock.patch.object(prune_alpine, "discover_targets", return_value=(qin_firmware.target,)),
        mock.patch.object(prune_alpine, "discover_profiles", return_value=("default",)),
        mock.patch.object(prune_alpine, "load_target", return_value=target),
        mock.patch.object(prune_alpine, "load_platform", return_value={}),
        mock.patch.object(prune_alpine, "selected_packages", return_value=()),
        mock.patch.object(prune_alpine, "current_release_context", return_value=release_context),
        mock.patch.object(prune_alpine, "alpine_rootfs_recipe", new=rootfs_recipe),
        mock.patch.object(prune_alpine, "environment_recipe_digest", return_value="a" * 64),
    ):
        initial = {entry.path: entry.action for entry in prune_alpine.rootfs_entries(cache)}
        assert initial == {f"rootfs/{first}": "protected", f"rootfs/{bare}": "protected"}

        qin_firmware.publish(report=b"report B\n")
        unrelated = selected_recipe()
        assert unrelated == first
        assert rootfs_state.receipt_matches(rootfs_state.rootfs_output(cache, first), unrelated)
        after_report = {entry.path: entry.action for entry in prune_alpine.rootfs_entries(cache)}
        assert after_report == initial

        qin_firmware.publish(system_calibration=b"system calibration B\x00")
        changed = selected_recipe()
        assert changed != first
        assert not rootfs_state.receipt_matches(rootfs_state.rootfs_output(cache, first), changed)
        output = rootfs_state.rootfs_output(cache, changed)
        output.mkdir()
        (output / "rootfs.cpio").write_bytes(b"new synthetic rootfs artifact\n")
        rootfs_state.write_receipt(output, changed)
        after_calibration = {
            entry.path: entry.action for entry in prune_alpine.rootfs_entries(cache)
        }
        assert after_calibration == {
            f"rootfs/{first}": "candidate",
            f"rootfs/{bare}": "protected",
            f"rootfs/{changed}": "protected",
        }
        assert discard_obsolete_rootfs(cache) == (f"rootfs/{first}",)
        assert not rootfs_state.rootfs_output(cache, first).exists()
        for recipe in (bare, changed):
            assert rootfs_state.receipt_matches(rootfs_state.rootfs_output(cache, recipe), recipe)

        source = alpine_source.root / "scripts/fplinux_cli/build/environment.py"
        source.write_bytes(b"changed build environment\n")
        updated = selected_recipe()
        updated_bare = rootfs_recipe(environment_recipe, signing_key, (), root_kind=root_kind)
        assert {updated, updated_bare}.isdisjoint({bare, changed})
        for recipe in (updated, updated_bare):
            output = rootfs_state.rootfs_output(cache, recipe)
            output.mkdir()
            (output / "rootfs.cpio").write_bytes(b"updated synthetic rootfs artifact\n")
            rootfs_state.write_receipt(output, recipe)

        after_source = {entry.path: entry.action for entry in prune_alpine.rootfs_entries(cache)}
        assert after_source == {
            f"rootfs/{bare}": "candidate",
            f"rootfs/{changed}": "candidate",
            f"rootfs/{updated}": "protected",
            f"rootfs/{updated_bare}": "protected",
        }
        assert set(discard_obsolete_rootfs(cache)) == {f"rootfs/{bare}", f"rootfs/{changed}"}
        for recipe in (updated, updated_bare):
            assert rootfs_state.receipt_matches(rootfs_state.rootfs_output(cache, recipe), recipe)
