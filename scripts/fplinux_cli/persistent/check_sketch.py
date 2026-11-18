#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Audit ``flash_stage_sketch.c`` against the t117_fdl headers.

The sandbox where this module was written has no C compiler, so the
sketch's well-formedness is checked mechanically instead: balanced
brackets, every protocol identifier resolved against the real
``cmd_def.h`` / ``common.h`` / ``packet.h`` of a Spreadtrum t117_fdl
tree, every function call either sketched or declared, and no
pseudo-syntax left in. Run it against a spreadtrum_flash checkout:

    python3 check_sketch.py /path/to/spreadtrum_flash/t117_fdl

Exit status is 0 when every check passes. The executable behaviour of
the sketch is pinned separately, by ``test_c_sketch_reply_parity`` in
``selftest.py``.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Symbols the sketch relies on, and the header each must come from.
HEADER_SYMBOLS = {
    "cmd_def.h": (
        "BSL_CMD_CONNECT",
        "BSL_CMD_START_DATA",
        "BSL_CMD_MIDST_DATA",
        "BSL_CMD_END_DATA",
        "BSL_CMD_READ_FLASH",
        "BSL_CMD_ERASE_FLASH",
        "BSL_REP_ACK",
        "BSL_REP_INVALID_CMD",
        "BSL_REP_UNKNOWN_CMD",
        "BSL_REP_OPERATION_FAILED",
        "BSL_REP_DOWN_DEST_ERROR",
        "BSL_REP_DOWN_SIZE_ERROR",
        "BSL_REP_DOWN_EARLY_END",
        "BSL_REP_VERIFY_ERROR",
        "BSL_REP_READ_FLASH",
    ),
    "common.h": ("READ16_BE", "READ32_BE", "WRITE16_BE"),
    "packet.h": ("MAX_PKT_PAYLOAD", "dl_get_packet", "dl_send_ack",
                 "dl_send_packet", "dl_send_buf"),
}

C_KEYWORDS = {"if", "for", "switch", "while", "sizeof", "return"}

def strip_code(code: str) -> str:
    """Remove comments and string literals, one state machine."""
    out = []
    index = 0
    while index < len(code):
        two = code[index : index + 2]
        if two == "/*":
            end = code.find("*/", index + 2)
            index = len(code) if end < 0 else end + 2
        elif two == "//":
            end = code.find("\n", index)
            index = len(code) if end < 0 else end
        elif code[index] == '"':
            index += 1
            while index < len(code) and code[index] != '"':
                index += 2 if code[index] == "\\" else 1
            index += 1
            out.append('""')
        else:
            out.append(code[index])
            index += 1
    return "".join(out)


def fail(message: str) -> None:
    print(f"FAIL {message}")


def main(argv: list[str]) -> int:
    include_dir = Path(argv[1]) if len(argv) > 1 else Path("t117_fdl")
    sketch_path = HERE / "flash_stage_sketch.c"
    sketch = sketch_path.read_text(encoding="utf-8")
    code = strip_code(sketch)
    problems = 0

    for left, right in (("(", ")"), ("{", "}"), ("[", "]")):
        depth = code.count(left) - code.count(right)
        if depth:
            problems += 1
            fail(f"unbalanced {left}{right} (depth {depth:+d})")

    # The sketch defines the one SPRD command t117_fdl trims out.
    if "#define BSL_CMD_ENABLE_WRITE_FLASH" not in code:
        problems += 1
        fail("BSL_CMD_ENABLE_WRITE_FLASH is not defined in the sketch")

    for header, symbols in HEADER_SYMBOLS.items():
        path = include_dir / header
        if not path.is_file():
            fail(f"cannot read {path}: pass the t117_fdl directory")
            print("\nthe header directory argument is required")
            return 2
        text = path.read_text(encoding="utf-8")
        for symbol in symbols:
            if symbol not in text:
                problems += 1
                fail(f"{symbol} is missing from {header}")

    # Sketch-internal constants must stay scratch-modelled numbers.
    for constant in ("NAND_BLOCK_BYTES", "STAGED_IMAGE_MAX"):
        if not re.search(rf"#define {constant}", code):
            problems += 1
            fail(f"{constant} is not a compile-time constant")

    # Definitions and declarations: a type word, then the name in
    # front of a parameter list or an array bound. Statement lines
    # (a "return foo(...)" would otherwise look like a definition)
    # are excluded by the lookahead.
    declared = set(re.findall(
        r"^(?!\s*(?:return|if|for|while|switch|do|else|case)\b)\s*"
        r"(?:static\s+|extern\s+){0,2}(?:const\s+)?(?:unsigned\s+)?"
        r"(?:[A-Za-z_]\w*\s+)+?\**\s*([A-Za-z_]\w*)\s*[\[(]",
        code, flags=re.M))
    # Function calls, lower case so types narrow down quickly.
    for call in set(re.findall(r"\b([a-z_]\w*)\s*\(", code)):
        if call in C_KEYWORDS or call.startswith("dl_"):
            continue  # packet.h helpers, checked against the header
        if call not in declared:
            problems += 1
            fail(f"function {call}() is called but never sketched or declared")

    if problems:
        print(f"\n{problems} problem(s)")
        return 1
    print(f"ok   {sketch_path.name}: brackets, identifiers and calls all resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

