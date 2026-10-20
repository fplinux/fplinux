# SPDX-License-Identifier: GPL-2.0-only
"""Keep cached C checks bound to the delivered profile's generation inputs."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.quality.inputs import check_scope_closure_digest
from fplinux_cli.quality.receipts import (
    CheckReceiptRecipe,
    check_scope_receipt_recipe,
    publish_success_receipt,
    receipt_matches,
)
from fplinux_cli.workspace.capture import WorkspaceFile, WorkspaceSnapshot

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize(
    "changed_input",
    [
        "lib/fplinux/fplinux-ui-profile.json",
        "lib/fplinux/fplinux-ui-profile.c",
        "scripts/fplinux_ui_profile.py",
    ],
    ids=["tokens", "delivered-table", "generator"],
)
def test_c_receipt_misses_after_a_profile_generation_input_changes(
    tmp_path: Path, changed_input: str
) -> None:
    """Profile edits revoke success, while unrelated documentation remains reusable."""
    inputs = {
        "lib/fplinux/fplinux-ui-profile.json": b'{"margin": 4}\n',
        "lib/fplinux/fplinux-ui-profile.c": b"const int margin = 4;\n",
        "scripts/fplinux_ui_profile.py": b'print("generate profile")\n',
        "README.md": b"Documentation\n",
    }

    def recipe(contents: dict[str, bytes]) -> CheckReceiptRecipe:
        snapshot = WorkspaceSnapshot(
            tuple(WorkspaceFile(path, data, 0o644) for path, data in contents.items()),
            "a" * 64,
        )
        return check_scope_receipt_recipe(
            "c",
            check_scope_closure_digest("c", snapshot),
            environment_generation="b" * 64,
            orchestration_recipe="c" * 64,
        )

    publish_success_receipt(tmp_path, recipe(inputs))
    assert receipt_matches(tmp_path, recipe(inputs))
    unrelated = {**inputs, "README.md": b"Changed documentation\n"}
    assert receipt_matches(tmp_path, recipe(unrelated))
    changed = {**inputs, changed_input: inputs[changed_input] + b"\n"}
    assert not receipt_matches(tmp_path, recipe(changed))
