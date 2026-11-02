#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A local CLI peer for preflight orchestration, without network or installation."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

arguments = sys.argv[1:]
root = Path.cwd()
report = Path(os.environ["FPLINUX_PREFLIGHT_TEST_REPORT"])
record = {
    "arguments": arguments,
    "checkout": str(root),
    "data_directory": os.environ.get("FPLINUX_DATA_DIR"),
    "readme": (root / "README.md").read_text(),
    "cache_files": sorted(str(path.relative_to(root)) for path in (root / ".data").rglob("*")),
}
with report.open("a") as stream:
    stream.write(json.dumps(record) + "\n")
stage = " ".join(arguments[:2]) if arguments[0] == "dependencies" else arguments[0]
if os.environ.get("FPLINUX_PREFLIGHT_TEST_FAIL") == stage:
    sys.exit(23)
if arguments[:2] == ["dependencies", "create"]:
    originals = Path(arguments[2])
    originals.mkdir()
    manifest: dict[str, object] = {
        "snapshot": "363e25cc2c7c33c5cad19cb52c53de0c36f3d72c6ee3915f107258ef99f3ed1a",
        "context": {},
        "inputs": [],
        "environment": None,
    }
    (originals / "manifest.json").write_text(json.dumps(manifest))
elif arguments == ["setup", "--offline"]:
    state = root / ".data/environment/state.json"
    state.parent.mkdir(parents=True)
    state.write_text(json.dumps({"recipe": "a" * 64, "generation": "b" * 64, "content": "c" * 64}))
elif arguments != ["doctor"] and arguments[:2] != ["dependencies", "verify"]:
    sys.exit(24)
