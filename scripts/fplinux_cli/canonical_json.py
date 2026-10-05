# SPDX-License-Identifier: GPL-2.0-only
"""Order JSON fields through a comment-preserving parser."""

from __future__ import annotations

import subprocess
from pathlib import Path, PurePosixPath


def normalize_json(relative: str, contents: bytes) -> bytes:
    """Retain values and arrays while ordering descriptions and independent maps."""
    relative = relative.removesuffix(".in")
    path = PurePosixPath(relative)
    if path.suffix not in {".json", ".jsonc"} or path.name == "package-lock.json":
        return contents
    contents.decode("utf-8")
    bridge = Path(__file__).with_name("canonical_json_tree.mjs")
    try:
        result = subprocess.run(
            ["node", str(bridge), relative],
            input=contents,
            capture_output=True,
            check=False,
            timeout=30,
        )
    except subprocess.TimeoutExpired as error:
        msg = f"{relative}: JSON field ordering timed out"
        raise ValueError(msg) from error
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        msg = f"{relative}: {detail or 'JSON field ordering failed'}"
        raise ValueError(msg)
    return result.stdout
