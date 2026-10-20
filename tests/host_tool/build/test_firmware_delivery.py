# SPDX-License-Identifier: GPL-2.0-only
"""Build-stage firmware delivery from a selected immutable source snapshot."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build import __main__ as build_stages
from fplinux_cli.build import inputs as build_source_inputs
from fplinux_cli.build import process as build_process
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.workspace import build_inputs, staging

from tests.small.device_data.firmware_fixtures import (
    qin_firmware as qin_firmware,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.small.workspace.workspace_fixtures import (
    empty_package_graph,
    write_registration_source,
    write_source_lock_files,
)

if TYPE_CHECKING:
    from pathlib import Path

    from tests.small.device_data.firmware_fixtures import QinFirmwareContext


class RootfsBoundaryReachedError(Exception):
    """Stop before any Alpine process or later build stage can execute."""


@pytest.fixture
def firmware_workspace(qin_firmware: QinFirmwareContext, platform: str, root_kind: str) -> Path:
    """Capture with the case's delivery policy and controlled source declarations."""
    root = qin_firmware.root
    inventory = [
        write_registration_source(root),
        *(
            (relative.replace("platforms/demo/", f"platforms/{platform}/"), source)
            for relative, source in write_source_lock_files(root, qin_firmware.target)
        ),
    ]
    target = {
        "platform": platform,
        "linux": {"root": {"kind": root_kind}},
        "device_data": {"groups": qin_firmware.groups},
    }
    with (
        mock.patch.object(build_inputs, "ROOT", root),
        mock.patch.object(staging, "ROOT", root),
        mock.patch.object(build_inputs, "load_target", return_value=target),
        mock.patch.object(build_inputs, "load_platform", return_value={}),
        mock.patch.object(build_inputs, "target_build_source_files", return_value=inventory),
        mock.patch.object(
            build_inputs,
            "shared_linux_source_files",
            return_value=build_inputs.SharedLinuxSources(),
        ),
        empty_package_graph(),
    ):
        snapshot = build_inputs.target_workspace_snapshot(qin_firmware.target)
        workspace = staging.stage_workspace_snapshot(snapshot)
    # A later valid selection must not alter an already staged build's inputs.
    qin_firmware.publish(system_calibration=b"system calibration B\x00")
    return workspace


@pytest.mark.parametrize(
    ("platform", "root_kind", "rootfs_delivery"),
    [
        pytest.param("sc9820e", "initramfs", True, id="qin-ram-rootfs"),
        pytest.param("ums9117", "initramfs", False, id="ums-ram-runtime-data"),
        pytest.param("ums9117", "external", True, id="external-rootfs"),
    ],
)
def test_build_stage_delivers_snapshot_bytes_at_the_selected_loader_boundary(  # noqa: PLR0913 -- owned fixtures and explicit delivery policies.
    qin_firmware: QinFirmwareContext,
    firmware_workspace: Path,
    monkeypatch: pytest.MonkeyPatch,
    platform: str,
    root_kind: str,
    *,
    rootfs_delivery: bool,
) -> None:
    """Execute configuration and snapshot recapture, stopping at Alpine execution."""
    cpio = shutil.which("cpio")
    if cpio is None:
        pytest.fail("GNU cpio is required to inspect the build-stage capsule")
    expected = {
        "wcnmodem.bin": b"radio payload\x00\x01",
        "qin1splus/system-config.bin": b"system configuration\x00",
        "qin1splus/system-cali.bin": b"system calibration A\x00",
        "qin1splus/wcn-config.bin": b"wcn configuration\x00",
        "qin1splus/wcn-cali.bin": b"wcn calibration\x00",
    }
    snapshot_firmware = firmware_workspace / (
        f".fplinux-inputs/device-data/{qin_firmware.target}/groups/wcn-firmware"
    )
    if rootfs_delivery:
        captured = {
            path.relative_to(snapshot_firmware).as_posix(): path.read_bytes()
            for path in snapshot_firmware.rglob("*")
            if path.is_file()
        }
        assert captured == expected
    else:
        assert not snapshot_firmware.exists()
    current_calibration = (
        qin_firmware.cache
        / "device-data"
        / qin_firmware.target
        / "phone/groups/wcn-firmware/system-cali.bin"
    )
    assert current_calibration.read_bytes() == b"system calibration B\x00"
    target = {
        "platform": platform,
        "device_data": {"groups": qin_firmware.groups},
        "linux": {"root": {"kind": root_kind}},
        "image": (
            {
                "kind": "ext4-root",
                "filename": "root.ext4",
                "partuuid": "12345678-02",
                "label": "test-root",
                "uuid": "11111111-2222-4333-8444-555555555555",
                "size": 67108864,
                "block_size": 4096,
                "inode_size": 256,
            }
            if root_kind == "external"
            else {"kind": "none"}
        ),
    }
    monkeypatch.setenv("FPLINUX_ENVIRONMENT_RECIPE", "a" * 64)
    monkeypatch.setenv("FPLINUX_ENVIRONMENT_CONTENT", "b" * 64)
    # main() sets the combined image identity; keep that mutation within this case.
    monkeypatch.setenv("FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE", "")
    monkeypatch.setattr(common, "ROOT", firmware_workspace)
    monkeypatch.setattr(build_source_inputs, "CACHE", qin_firmware.cache)
    monkeypatch.setattr(build_source_inputs, "OUTPUT", qin_firmware.root / "out")
    monkeypatch.setattr(
        sys, "argv", ["build-stages", "--target", qin_firmware.target, "--jobs", "1"]
    )
    with (
        mock.patch.object(build_process, "lower_build_priority"),
        mock.patch.object(RunReporter, "from_environment", return_value=None),
        mock.patch.object(build_stages, "load_target", return_value=target),
        mock.patch.object(
            build_stages, "load_platform", return_value={"linux": {"source_lock": "linux"}}
        ),
        mock.patch.object(build_stages, "selected_packages", return_value=()),
        mock.patch.object(build_stages, "bundle_packages", return_value=()),
        mock.patch.object(build_stages, "load_release", return_value={}),
        mock.patch.object(build_stages, "prepare_linux", return_value=(None, None)),
        mock.patch.object(
            build_stages, "build_rootfs", side_effect=RootfsBoundaryReachedError
        ) as rootfs,
        pytest.raises(RootfsBoundaryReachedError),
    ):
        build_stages.main()

    firmware = rootfs.call_args.kwargs["firmware"]
    delivered = {item.destination: item.contents for item in firmware}
    assert delivered == (expected if rootfs_delivery else {})
    assert len(firmware) == (5 if rootfs_delivery else 0)
    capsules = list((qin_firmware.root / "out").rglob("device-data.cpio"))
    if root_kind == "external":
        assert rootfs.call_args.kwargs["external_image"] == {
            "kind": "ext4-root",
            "filename": "root.ext4",
            "partuuid": "12345678-02",
            "label": "test-root",
            "uuid": "11111111-2222-4333-8444-555555555555",
            "size": 67108864,
            "block_size": 4096,
            "inode_size": 256,
        }
        assert rootfs.call_args.kwargs["external_output"] == (
            qin_firmware.root / "out/xiaomi-qin-1s-plus/builds/release/work/rootfs-image"
        )
        assert capsules == []
        return
    assert len(capsules) == 1
    assert capsules[0].stat().st_mode & 0o777 == 0o600
    extracted = qin_firmware.root / "extracted-capsule"
    extracted.mkdir()
    result = subprocess.run(
        [cpio, "--extract", "--make-directories", "--no-preserve-owner"],
        input=capsules[0].read_bytes(),
        cwd=extracted,
        env={**os.environ, "LC_ALL": "C"},
        capture_output=True,
        check=False,
        timeout=10,
    )
    assert result.returncode == 0, result.stderr.decode()
    files = {
        path.relative_to(extracted).as_posix(): path.read_bytes()
        for path in extracted.rglob("*")
        if path.is_file()
    }
    assert files == {}
    assert list(extracted.iterdir()) == []
