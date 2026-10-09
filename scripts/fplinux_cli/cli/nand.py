# SPDX-License-Identifier: GPL-2.0-only
"""Select the target NAND reader and report command progress."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, cast

from fplinux_cli.common import fail
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.runtime import bundle_session
from fplinux_cli.runtime.nand_backup import backup_nand, read_geometry_report

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.runtime.nand_backup import SessionFactory, SshTransport


def _target_reader(
    target: str, profile: str | None, build_type: str
) -> tuple[dict[str, Any], SessionFactory]:
    """Return the target's declared NAND reader and its exact current-profile session."""
    nand = load_target(target, profile=profile, build_type=build_type).get("nand")
    if nand is None:
        fail(f"NAND access is not supported for target {target}")

    def connect() -> tuple[SshTransport, dict[str, Any]]:
        ssh, session = bundle_session.current_target_ssh_session(
            target, profile=profile, build_type=build_type
        )
        return cast("SshTransport", ssh), session

    return nand, connect


def backup_target_nand(
    target: str,
    output: Path,
    *,
    profile: str | None = None,
    build_type: str = "release",
    reporter: RunReporter | None = None,
) -> Path:
    """Back up one supported target through its exact current-profile session."""
    nand, connect = _target_reader(target, profile, build_type)
    own_reporter = reporter is None
    if reporter is None:
        reporter = RunReporter.create("nand", target=target, verbose=False)
    with reporter.stage("read-nand"):
        result = backup_nand(
            connect,
            output,
            target=target,
            raw_device=nand["raw_device"],
            declared_id=nand.get("id"),
            declared_raw_page_bytes=nand.get("raw_page_bytes"),
        )
    if own_reporter:
        reporter.finish()
    return result


def identify_target_nand(
    target: str, *, profile: str | None = None, build_type: str = "release"
) -> None:
    """Print the chip identity and geometry that the target's running NAND reader reports."""
    nand, connect = _target_reader(target, profile, build_type)
    reporter = RunReporter.create("nand", target=target, verbose=False)
    with reporter.stage("identify-nand") as stage:
        ssh, session = connect()
        report = read_geometry_report(ssh, session, nand["raw_device"])
        stage.write(report.encode("ascii"))
    print(report, end="", flush=True)
    reporter.finish()
