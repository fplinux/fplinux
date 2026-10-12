# SPDX-License-Identifier: GPL-2.0-only
"""A small offline export with independently encoded receipt inputs."""

from __future__ import annotations

import hashlib
import json
from typing import TYPE_CHECKING

from tests.bundle_support import file_record

if TYPE_CHECKING:
    from pathlib import Path


def write_offline_export(
    root: Path,
    *,
    text: str = "Current documentation",
    phone_inputs: dict[str, bytes] | None = None,
) -> dict[str, bytes]:
    """Stand in for the separate Node build, retaining real receipt and file collection."""
    source_bytes = f"# {text}\n".encode()
    source = root / "site/src/content/docs/index.mdx"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(source_bytes)
    source.chmod(0o644)
    inputs = {
        "site/src/content/docs/index.mdx": source_bytes,
        "site/src/data/phones.json": b"[]\n",
    }
    inputs.update(phone_inputs or {})
    for relative, data in inputs.items():
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        destination.chmod(0o644)
    output = root / ".cache/out/site-offline"
    contents = {
        "index.html": f"<html><body>{text}</body></html>\n".encode(),
        "offline/runtime.js": b"window.offlineDocumentation = true;\n",
        "offline/styles.css": b"body { color: black; }\n",
        "images/phone.svg": b'<svg xmlns="http://www.w3.org/2000/svg"></svg>\n',
        "licenses/fonts/OFL.txt": b"Synthetic font redistribution notice\n",
    }
    for relative, data in contents.items():
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        destination.chmod(0o644)
    encoded_inputs = b"".join(
        relative.encode() + b"\0\x00\x00\x81\xa4" + inputs[relative] + b"\0"
        for relative in sorted(inputs)
    )
    receipt = {
        "inputs_sha256": hashlib.sha256(encoded_inputs).hexdigest(),
        "files": {relative: file_record(output / relative) for relative in contents},
    }
    (output.parent / "site-offline-receipt.json").write_text(
        json.dumps(receipt) + "\n", encoding="utf-8"
    )
    return {f"docs/{relative}": data for relative, data in contents.items()}
