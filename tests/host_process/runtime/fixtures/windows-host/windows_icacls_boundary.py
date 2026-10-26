# SPDX-License-Identifier: GPL-2.0-only
"""Apply the exercised icacls grant semantics to test-owned ACL metadata."""

import json
import os
import sys
from pathlib import Path


def main() -> None:
    """Replace native NTFS operations with persistent, controlled access rules."""
    state = Path(os.environ["FPLINUX_TEST_ACL_STATE"])
    acl = json.loads(state.read_text(encoding="utf-8"))
    assert Path(sys.argv[1]).exists()
    arguments = iter(sys.argv[2:])
    for argument in arguments:
        if argument == "/inheritance:r":
            acl["protected"] = True
            acl["rules"] = [rule for rule in acl["rules"] if not rule["inherited"]]
        elif argument == "/remove:g":
            sid = next(arguments).removeprefix("*")
            acl["rules"] = [
                rule for rule in acl["rules"] if rule["sid"] != sid or rule["type"] != "Allow"
            ]
        elif argument == "/grant:r":
            continue
        elif argument.startswith("*"):
            sid, rights = argument.removeprefix("*").split(":", 1)
            assert rights in {"F", "(OI)(CI)F"}
            acl["rules"] = [
                rule for rule in acl["rules"] if rule["sid"] != sid or rule["type"] != "Allow"
            ]
            acl["rules"].append({"sid": sid, "type": "Allow", "inherited": False})
        else:
            raise AssertionError("Unsupported icacls argument: " + argument)
    state.write_text(json.dumps(acl), encoding="utf-8")


if __name__ == "__main__":
    main()
