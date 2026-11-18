# SPDX-License-Identifier: GPL-2.0-only
"""Retain before and after diagnostics for an explicit read-only NAND identification."""

from __future__ import annotations

import shlex
import tempfile
from contextlib import nullcontext, suppress
from pathlib import PurePosixPath
from textwrap import dedent
from typing import TYPE_CHECKING, cast

from fplinux_cli.common import fail

from .nand_backup import GEOMETRY_TIMEOUT_SECONDS

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import Any, BinaryIO

    from .nand_backup import SshTransport


def _geometry_audit_command(attribute: str, raw_device: str) -> str:
    """Keep both audit snapshots around one reader open, including a failed open."""
    return dedent(
        """\
        nand_audit() {{
            if ! cat {audit} 2>/dev/null; then
                printf 'unavailable: cannot read NAND audit\\n'
            fi
        }}
        printf '\\036FPLINUX_NAND_AUDIT_BEFORE\\n'
        nand_audit
        printf '\\036FPLINUX_NAND_GEOMETRY\\n'
        if (cat {geometry} 3<{raw_device}); then
            nand_status=0
        else
            nand_status=$?
        fi
        printf '\\036FPLINUX_NAND_AUDIT_AFTER\\n'
        nand_audit
        printf '\\036FPLINUX_NAND_END\\n'
        exit "$nand_status"
        """
    ).format(
        audit=shlex.quote(str(PurePosixPath(attribute).with_name("audit"))),
        geometry=shlex.quote(attribute),
        raw_device=shlex.quote(raw_device),
    )


def _record_geometry_audit(encoded: bytes, diagnostics: Callable[[str], None]) -> bytes:
    """Retain returned snapshots even when SSH reports the reader's failure."""
    _prefix, before_marker, before = encoded.partition(b"\x1eFPLINUX_NAND_AUDIT_BEFORE\n")
    before, geometry_marker, geometry = before.partition(b"\x1eFPLINUX_NAND_GEOMETRY\n")
    geometry, after_marker, after = geometry.partition(b"\x1eFPLINUX_NAND_AUDIT_AFTER\n")
    after, end_marker, _suffix = after.partition(b"\x1eFPLINUX_NAND_END\n")
    for phase, snapshot, complete in (
        ("before", before, before_marker and geometry_marker),
        ("after", after, after_marker and end_marker),
    ):
        text = snapshot.decode("ascii", errors="replace").rstrip("\n")
        if not complete:
            missing = "unavailable: NAND audit capture did not complete"
            text = f"{text}\n{missing}" if text else missing
        diagnostics(f"NAND audit {phase}:\n{text}")
    return geometry


def read_identification_report(
    ssh: SshTransport,
    session: dict[str, Any],
    raw_device: str,
    *,
    diagnostics: Callable[[str], None],
) -> str:
    """Read geometry once and retain both audit snapshots independently of its status."""
    attribute = f"/sys/class/misc/{PurePosixPath(raw_device).name}/device/geometry"
    command = _geometry_audit_command(attribute, raw_device)
    with tempfile.TemporaryFile(mode="w+b") as report:
        stream_failed = True
        try:
            ssh.stream_remote(
                session,
                command,
                cast("BinaryIO", report),
                timeout=GEOMETRY_TIMEOUT_SECONDS,
            )
            stream_failed = False
        finally:
            # Log failures must not replace the original reader or transport error.
            with suppress(OSError) if stream_failed else nullcontext():
                report.seek(0)
                encoded = _record_geometry_audit(report.read(), diagnostics)
    try:
        return encoded.decode("ascii")
    except UnicodeDecodeError:
        fail("NAND geometry report is not ASCII text")
