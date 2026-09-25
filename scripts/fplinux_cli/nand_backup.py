# SPDX-License-Identifier: GPL-2.0-only
"""Identify the fitted NAND and publish a complete read-only backup with its geometry."""

from __future__ import annotations

import json
import os
import re
import shlex
import tempfile
from collections.abc import Callable
from dataclasses import asdict
from pathlib import Path, PurePosixPath
from typing import Any, BinaryIO, Protocol, cast

from fplinux_cli.cli import runtime as runtime_commands
from fplinux_cli.manifests.targets import load_target

from .common import canonical_json_bytes, fail, replace_file_atomically, sha256_bytes, sha256_file
from .device_data import NandGeometry
from .output import RunReporter

BACKUP_TIMEOUT_SECONDS = 15 * 60
GEOMETRY_TIMEOUT_SECONDS = 60
_GEOMETRY_REPORT_KEYS = frozenset(
    {
        "id_bytes",
        "chip",
        "page_main_bytes",
        "oob_bytes",
        "pages_per_block",
        "block_count",
        "raw_bytes",
        "geometry_source",
        "feature_a0",
        "feature_b0",
        "feature_c0",
    }
)
_GEOMETRY_SIZE_KEYS = (
    "page_main_bytes",
    "oob_bytes",
    "pages_per_block",
    "block_count",
    "raw_bytes",
)
_RECEIPT_KEYS = frozenset({"id_bytes", "chip", *_GEOMETRY_SIZE_KEYS, "sha256", "target"})
_ID_BYTES = re.compile(r"(?:[0-9a-f]{2})+")
_DECIMAL = re.compile(r"[0-9]+")
_SHA256 = re.compile(r"[0-9a-f]{64}")


class SshTransport(Protocol):
    """Authenticated binary-stream operation supplied by the selected bundle."""

    def stream_remote(
        self,
        session: dict[str, Any],
        command: str,
        destination: BinaryIO,
        *,
        timeout: float,
    ) -> None:
        """Write remote stdout directly into the supplied local file."""


SessionFactory = Callable[[], tuple[SshTransport, dict[str, Any]]]


def backup_receipt_path(backup: Path) -> Path:
    """Return the geometry receipt path that belongs beside one backup file."""
    return backup.with_name(f"{backup.name}.json")


def _checked_geometry(geometry: NandGeometry, source: str) -> NandGeometry:
    """Require an identified chip whose total size matches its page layout."""
    if _ID_BYTES.fullmatch(geometry.id_bytes) is None:
        fail(f"{source} has invalid id_bytes: {geometry.id_bytes!r}")
    if not geometry.chip:
        fail(f"{source} has an empty chip name")
    layout = (
        geometry.page_main_bytes,
        geometry.oob_bytes,
        geometry.pages_per_block,
        geometry.block_count,
    )
    expected_raw_bytes = geometry.raw_page_bytes * geometry.pages_per_block * geometry.block_count
    if min(layout) < 1 or geometry.raw_bytes != expected_raw_bytes:
        fail(
            f"{source} is inconsistent: {geometry.page_main_bytes}+{geometry.oob_bytes}-byte "
            f"pages, {geometry.pages_per_block} pages per block, {geometry.block_count} blocks, "
            f"{geometry.raw_bytes} bytes"
        )
    return geometry


def parse_geometry_report(report: str) -> NandGeometry:
    """Admit the reader's key=value report only for an identified, consistent chip."""
    values: dict[str, str] = {}
    for line in report.splitlines():
        key, separator, value = line.partition("=")
        if not separator or key not in _GEOMETRY_REPORT_KEYS or key in values:
            fail(f"invalid NAND geometry report line: {line!r}")
        values[key] = value
    missing = _GEOMETRY_REPORT_KEYS - set(values)
    if missing:
        fail(f"NAND geometry report lacks: {', '.join(sorted(missing))}")
    if values["geometry_source"] == "unknown":
        fail(
            f"the running kernel does not identify this NAND chip: "
            f"id_bytes={values['id_bytes']} feature_a0={values['feature_a0']} "
            f"feature_b0={values['feature_b0']} feature_c0={values['feature_c0']}"
        )
    if values["geometry_source"] not in {"dt", "table"}:
        fail(f"NAND geometry report has an invalid geometry_source: {values['geometry_source']}")
    for key in _GEOMETRY_SIZE_KEYS:
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
    return _checked_geometry(geometry, "NAND geometry report")


def require_declared_chip(
    geometry: NandGeometry,
    *,
    target: str,
    declared_id: int | None,
    declared_raw_page_bytes: int | None,
    source: str,
) -> None:
    """Reject a reported chip that contradicts the target's optional declared expectation."""
    if declared_id is None or declared_raw_page_bytes is None:
        return
    # The declared ID holds the manufacturer byte in bits 7:0 and the device byte in 15:8.
    declared_id_bytes = declared_id.to_bytes(2, "little").hex()
    if (geometry.id_bytes, geometry.raw_page_bytes) != (
        declared_id_bytes,
        declared_raw_page_bytes,
    ):
        fail(
            f"{source} does not match target {target}: reported id_bytes={geometry.id_bytes} "
            f"with {geometry.raw_page_bytes}-byte pages; target declares "
            f"id_bytes={declared_id_bytes} with {declared_raw_page_bytes}-byte pages"
        )


def _geometry_report(ssh: SshTransport, session: dict[str, Any], raw_device: str) -> str:
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


def read_backup_geometry(backup: Path, raw: bytes) -> NandGeometry | None:
    """Return the device-reported geometry recorded for exactly these backup bytes."""
    receipt = backup_receipt_path(backup)
    if not receipt.exists() and not receipt.is_symlink():
        return None
    if receipt.is_symlink() or not receipt.is_file():
        fail(f"NAND backup receipt is not a regular file: {receipt}")
    try:
        value = json.loads(receipt.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"NAND backup receipt cannot be read: {receipt}: {error}")
    if not isinstance(value, dict) or set(value) != _RECEIPT_KEYS:
        fail(f"NAND backup receipt must contain exactly: {', '.join(sorted(_RECEIPT_KEYS))}")
    for key in ("id_bytes", "chip", "sha256", "target"):
        if not isinstance(value[key], str):
            fail(f"NAND backup receipt {key} must be a string: {receipt}")
    for key in _GEOMETRY_SIZE_KEYS:
        if type(value[key]) is not int:
            fail(f"NAND backup receipt {key} must be an integer: {receipt}")
    if _SHA256.fullmatch(value["sha256"]) is None:
        fail(f"NAND backup receipt sha256 must be a lowercase SHA-256 digest: {receipt}")
    geometry = _checked_geometry(
        NandGeometry(
            id_bytes=value["id_bytes"],
            chip=value["chip"],
            page_main_bytes=value["page_main_bytes"],
            oob_bytes=value["oob_bytes"],
            pages_per_block=value["pages_per_block"],
            block_count=value["block_count"],
            raw_bytes=value["raw_bytes"],
        ),
        f"NAND backup receipt {receipt}",
    )
    if geometry.raw_bytes != len(raw):
        fail(
            f"NAND backup receipt {receipt} describes a {geometry.raw_bytes}-byte backup; "
            f"{backup} has {len(raw)} bytes"
        )
    if value["sha256"] != sha256_bytes(raw):
        fail(f"NAND backup receipt {receipt} describes different backup bytes than {backup}")
    return geometry


def backup_nand(  # noqa: PLR0913 -- the reader and the declared chip are distinct inputs.
    connect: SessionFactory,
    output: Path,
    *,
    target: str,
    raw_device: str,
    declared_id: int | None = None,
    declared_raw_page_bytes: int | None = None,
) -> Path:
    """Acquire one exact session and publish a complete physical NAND image and its receipt."""
    if output.name in {"", ".", ".."}:
        fail("NAND backup output must name a file")
    parent = output.parent
    if parent.is_symlink() or not parent.is_dir():
        fail(f"NAND backup output directory is missing or invalid: {parent}")
    if output.is_symlink() or (output.exists() and not output.is_file()):
        fail(f"NAND backup output is not a regular file: {output}")
    receipt = backup_receipt_path(output)
    if receipt.is_symlink() or (receipt.exists() and not receipt.is_file()):
        fail(f"NAND backup receipt is not a regular file: {receipt}")

    ssh, session = connect()
    geometry = parse_geometry_report(_geometry_report(ssh, session, raw_device))
    require_declared_chip(
        geometry,
        target=target,
        declared_id=declared_id,
        declared_raw_page_bytes=declared_raw_page_bytes,
        source="the phone's NAND",
    )
    read_batch_bytes = 30 * geometry.raw_page_bytes
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=parent,
            prefix=f".{output.name}.",
            mode="w+b",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            temporary.chmod(0o600)
            ssh.stream_remote(
                session,
                f"exec dd if={shlex.quote(raw_device)} bs={read_batch_bytes}",
                cast("BinaryIO", stream),
                timeout=BACKUP_TIMEOUT_SECONDS,
            )
            stream.flush()
            os.fsync(stream.fileno())

        actual_size = temporary.stat().st_size
        if actual_size != geometry.raw_bytes:
            fail(
                f"incomplete raw NAND image: expected {geometry.raw_bytes} bytes, "
                f"got {actual_size}"
            )
        digest = sha256_file(temporary)
        temporary.replace(output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)

    record = {**asdict(geometry), "sha256": digest, "target": target}
    replace_file_atomically(receipt, canonical_json_bytes(record), 0o600)
    print(
        f"NAND backup saved: {output} ({geometry.raw_bytes} bytes, sha256={digest})",
        flush=True,
    )
    print(
        f"NAND geometry saved: {receipt} (chip {geometry.chip}, id_bytes {geometry.id_bytes})",
        flush=True,
    )
    return output


def _target_reader(target: str, profile: str | None) -> tuple[dict[str, Any], SessionFactory]:
    """Return the target's declared NAND reader and its exact current-profile session."""
    nand = load_target(target, profile=profile).get("nand")
    if nand is None:
        fail(f"NAND access is not supported for target {target}")

    def connect() -> tuple[SshTransport, dict[str, Any]]:
        ssh, session = runtime_commands.current_target_ssh_session(target, profile=profile)
        return cast("SshTransport", ssh), session

    return nand, connect


def backup_target_nand(
    target: str,
    output: Path,
    *,
    profile: str | None = None,
    reporter: RunReporter | None = None,
) -> Path:
    """Back up one supported target through its exact current-profile session."""
    nand, connect = _target_reader(target, profile)
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


def identify_target_nand(target: str, *, profile: str | None = None) -> None:
    """Print the chip identity and geometry that the target's running NAND reader reports."""
    nand, connect = _target_reader(target, profile)
    reporter = RunReporter.create("nand", target=target, verbose=False)
    with reporter.stage("identify-nand") as stage:
        ssh, session = connect()
        report = _geometry_report(ssh, session, nand["raw_device"])
        stage.write(report.encode("ascii"))
    print(report, end="", flush=True)
    reporter.finish()
