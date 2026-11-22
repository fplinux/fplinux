# SPDX-License-Identifier: GPL-2.0-only
"""Acquire a raw NAND backup over the selected authenticated SSH session."""

from __future__ import annotations

import re
import shlex
import tempfile
from pathlib import PurePosixPath
from typing import TYPE_CHECKING, Protocol, cast

from fplinux_cli.common import fail
from fplinux_cli.device_data.nand_backup import (
    backup_receipt_path,
    require_backup_destination,
    write_backup,
)
from fplinux_cli.device_data.nand_geometry import (
    GEOMETRY_SIZE_KEYS,
    KONROW_NAND_TARGET,
    NandGeometry,
    require_consistent_geometry,
    require_declared_chip,
    require_konrow_geometry,
)

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path
    from typing import Any, BinaryIO

BACKUP_TIMEOUT_SECONDS = 15 * 60
GEOMETRY_TIMEOUT_SECONDS = 60
_GEOMETRY_REPORT_KEYS = frozenset(
    {
        "id_bytes",
        "chip",
        *GEOMETRY_SIZE_KEYS,
        "geometry_source",
        "feature_a0",
        "feature_b0",
        "feature_c0",
    }
)
_SPI_FEATURE_KEYS = ("feature_a0", "feature_b0", "feature_c0")
_DECIMAL = re.compile(r"[0-9]+")


class SshTransport(Protocol):
    """Authenticated binary-stream operation supplied by the selected bundle."""

    def stream_remote(
        self,
        session: dict[str, Any],
        command: str,
        destination: BinaryIO,
        *,
        timeout: float,
        progress: Callable[[int], None] | None = None,
    ) -> None:
        """Write remote stdout directly into the supplied local file."""


type SessionFactory = Callable[[], tuple[SshTransport, dict[str, Any]]]


def parse_geometry_report(report: str, *, target: str) -> NandGeometry:
    """Admit an identified chip and its physical layout for the selected target."""
    values: dict[str, str] = {}
    for line in report.splitlines():
        key, separator, value = line.partition("=")
        if not separator or key not in _GEOMETRY_REPORT_KEYS or key in values:
            fail(f"invalid NAND geometry report line: {line!r}")
        values[key] = value
    feature_keys = set(_SPI_FEATURE_KEYS)
    missing = _GEOMETRY_REPORT_KEYS - feature_keys - set(values)
    if missing:
        fail(f"NAND geometry report lacks: {', '.join(sorted(missing))}")
    source = values["geometry_source"]
    features = set(values) & feature_keys
    if source == "parallel" and features:
        fail("parallel NAND geometry report contains SPI features")
    if features and features != feature_keys:
        missing_features = feature_keys - features
        fail(f"NAND geometry report lacks: {', '.join(sorted(missing_features))}")
    if source == "unknown":
        feature_report = "".join(
            f" {key}={values[key]}" for key in _SPI_FEATURE_KEYS if key in values
        )
        fail(
            f"the running kernel does not identify this NAND chip: "
            f"id_bytes={values['id_bytes']}{feature_report}; raw NAND reads are unavailable"
        )
    if source != "parallel" and not features:
        fail(f"NAND geometry report lacks: {', '.join(_SPI_FEATURE_KEYS)}")
    if source not in {"dt", "table", "parallel"}:
        fail(f"NAND geometry report has an invalid geometry_source: {source}")
    if (source == "parallel") != (target == KONROW_NAND_TARGET):
        fail(f"NAND geometry report source {source!r} does not match target {target}")
    for key in GEOMETRY_SIZE_KEYS:
        if _DECIMAL.fullmatch(values[key]) is None:
            fail(f"NAND geometry report has a non-decimal {key}: {values[key]!r}")
    geometry = NandGeometry(
        id_bytes=values["id_bytes"],
        chip=values["chip"],
        page_main_bytes=int(values["page_main_bytes"]),
        oob_bytes=int(values["oob_bytes"]),
        pages_per_block=int(values["pages_per_block"]),
        block_count=int(values["block_count"]),
        raw_bytes=int(values["raw_bytes"]),
    )
    require_consistent_geometry(geometry, "NAND geometry report")
    if source == "parallel":
        try:
            require_konrow_geometry(geometry)
        except ValueError as error:
            fail(str(error))
    return geometry


def read_geometry_report(ssh: SshTransport, session: dict[str, Any], raw_device: str) -> str:
    """Open the raw reader, which identifies the chip, and read the geometry it reports."""
    attribute = f"/sys/class/misc/{PurePosixPath(raw_device).name}/device/geometry"
    # The redirection keeps the reader open while its sibling attribute is read.
    command = f"cat {shlex.quote(attribute)} 3<{shlex.quote(raw_device)}"
    with tempfile.TemporaryFile(mode="w+b") as report:
        ssh.stream_remote(
            session,
            command,
            cast("BinaryIO", report),
            timeout=GEOMETRY_TIMEOUT_SECONDS,
        )
        report.seek(0)
        encoded = report.read()
    try:
        return encoded.decode("ascii")
    except UnicodeDecodeError:
        fail("NAND geometry report is not ASCII text")


def backup_nand(  # noqa: PLR0913 -- the reader and the declared chip are distinct inputs.
    connect: SessionFactory,
    output: Path,
    *,
    target: str,
    acquisition_recipe: str,
    raw_device: str,
    declared_id: int | None = None,
    declared_raw_page_bytes: int | None = None,
    progress: Callable[[int, int], None] | None = None,
) -> Path:
    """Acquire one exact session and publish a complete physical NAND image and its receipt."""
    require_backup_destination(output, acquisition_recipe)
    ssh, session = connect()
    geometry = parse_geometry_report(read_geometry_report(ssh, session, raw_device), target=target)
    require_declared_chip(
        geometry,
        target=target,
        declared_id=declared_id,
        declared_raw_page_bytes=declared_raw_page_bytes,
        source="the phone's NAND",
    )
    read_batch_bytes = 30 * geometry.raw_page_bytes

    def read_progress(completed: int) -> None:
        if progress is not None and completed < geometry.raw_bytes:
            progress(completed, geometry.raw_bytes)

    def read_image(stream: BinaryIO) -> None:
        command = f"exec dd if={shlex.quote(raw_device)} bs={read_batch_bytes}"
        if progress is None:
            ssh.stream_remote(session, command, stream, timeout=BACKUP_TIMEOUT_SECONDS)
        else:
            progress(0, geometry.raw_bytes)
            ssh.stream_remote(
                session, command, stream, timeout=BACKUP_TIMEOUT_SECONDS, progress=read_progress
            )

    digest = write_backup(
        output,
        geometry,
        target=target,
        acquisition_recipe=acquisition_recipe,
        write_image=read_image,
    )
    if progress is not None:
        progress(geometry.raw_bytes, geometry.raw_bytes)
    print(
        f"NAND backup saved: {output} ({geometry.raw_bytes} bytes, sha256={digest})",
        flush=True,
    )
    print(
        f"NAND geometry saved: {backup_receipt_path(output)} "
        f"(chip {geometry.chip}, id_bytes {geometry.id_bytes})",
        flush=True,
    )
    return output
