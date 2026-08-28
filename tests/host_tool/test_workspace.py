# SPDX-License-Identifier: GPL-2.0-only
"""Host-tool test for Git-backed source inventory selection."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import workspace as workspace_module

from tests.process import run_process

_GIT_TIMEOUT_SECONDS = 10


class WorkspaceGitInventoryTests(unittest.TestCase):
    """Run the Git inventory consumer against a real temporary repository."""

    def test_quality_inventory_uses_git_excludes(self) -> None:
        """Machine-local excluded files do not enter the staged quality inventory."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            initialized = run_process(
                ["git", "init", "--quiet", str(root)],
                name="temporary Git initialization",
                timeout=_GIT_TIMEOUT_SECONDS,
            )
            self.assertEqual(initialized.returncode, 0, initialized.stderr)
            source = root / "source.py"
            source.write_text("source\n", encoding="utf-8")
            local = root / "local-tools/settings.json"
            local.parent.mkdir(parents=True)
            local.write_text("{}\n", encoding="utf-8")
            (root / ".git/info/exclude").write_text("/local-tools/\n", encoding="utf-8")
            added = run_process(
                ["git", "-C", str(root), "add", "--", "source.py"],
                name="temporary Git add",
                timeout=_GIT_TIMEOUT_SECONDS,
            )
            self.assertEqual(added.returncode, 0, added.stderr)

            with mock.patch.object(workspace_module, "ROOT", root):
                files = workspace_module.quality_files(enforce_source_policy=True)

            self.assertEqual(files, [("source.py", source)])


if __name__ == "__main__":
    unittest.main()
