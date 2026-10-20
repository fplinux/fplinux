# SPDX-License-Identifier: GPL-2.0-only
"""Prepare pinned loader assets for a target bundle."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING

from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.common import fail, replace_file_atomically, sha256_bytes

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path
    from typing import Any


def write_checked(data: bytes, destination: Path, expected: str) -> None:
    """Atomically write bytes that match their declared digest."""
    actual = sha256_bytes(data)
    if actual != expected:
        fail(f"{destination.name} SHA256 mismatch: expected {expected}, got {actual}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    replace_file_atomically(destination, data, 0o600, sync=False)


def _asset_lock_bytes(sources: Sequence[dict[str, Any]]) -> bytes:
    """Encode a self-contained lock retaining source and output integrity pins."""
    license_tag = "SPDX-License-" + "Identifier"
    lines = [f"# {license_tag}: GPL-2.0-only"]
    for source in sources:
        lines.extend(("", "[[source]]"))
        lines.extend(
            f"{field} = {json.dumps(source[field], ensure_ascii=False)}"
            for field in ("id", "kind", "url", "sha256", "cache_name", "license")
        )
        for output in source["output"]:
            lines.extend(("", "[[source.output]]"))
            lines.extend(
                f"{field} = {json.dumps(output[field], ensure_ascii=False)}"
                for field in ("role", "path", "sha256")
            )
    return ("\n".join(lines) + "\n").encode("utf-8")


def build_assets(
    sources: Sequence[dict[str, Any]], output: Path, *, lock_output: Path
) -> dict[str, tuple[str, str]]:
    """Fetch pinned assets and emit the resolved lock delivered beside them."""
    result: dict[str, tuple[str, str]] = {}
    for source in sources:
        archive = sources_build.fetch(
            source["url"],
            source["sha256"],
            inputs_build.CACHE / "downloads",
            source["cache_name"],
        )
        for item in source["output"]:
            expected = item["sha256"]
            data = archive.read_bytes()
            destination = output / item["path"]
            write_checked(data, destination, expected)
            result[item["role"]] = (item["path"], expected)
            process_build.log_message(f"{expected}  {destination}")
    lock_output.parent.mkdir(parents=True, exist_ok=True)
    replace_file_atomically(lock_output, _asset_lock_bytes(sources), 0o600, sync=False)
    return result
