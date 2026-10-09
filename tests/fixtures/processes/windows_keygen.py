# SPDX-License-Identifier: GPL-2.0-only
"""Replace the unavailable native key generator with a fixed valid SSH key blob."""

import base64
import sys
from pathlib import Path

destination = Path(sys.argv[sys.argv.index("-f") + 1])
destination.write_text("fixture private material\n", encoding="ascii")
public_blob = b"\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20" + b"k" * 32
destination.with_suffix(".pub").write_text(
    "ssh-ed25519 " + base64.b64encode(public_blob).decode("ascii") + " fixture\n",
    encoding="ascii",
)
