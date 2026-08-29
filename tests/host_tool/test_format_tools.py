# SPDX-License-Identifier: GPL-2.0-only
"""Actual pinned-tool coverage for mixed project source formatting."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from fplinux_cli.common import ROOT
from fplinux_cli.format import formatter_commands
from fplinux_cli.source_formats import classify_source_formats

from tests.process import run_process


class PinnedFormatterToolTests(unittest.TestCase):
    """Run every existing formatter against one private mixed projection."""

    def test_pinned_tools_format_their_current_source_boundaries(self) -> None:
        """Each routed source reaches its real tool and obtains canonical bytes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in (".clang-format", ".editorconfig", ".prettierrc.json", "pyproject.toml"):
                shutil.copy2(ROOT / name, root / name)
            contents = {
                "tool.py": "value=  1\n",
                "README.md": "# Demo\n\n-   item\n",
                "data.json": '{"value":1}\n',
                "target.toml": 'name="demo"\n',
                "script.sh": "#!/bin/sh\nif true;then\n echo ok\nfi\n",
                "helper": "#!/usr/bin/env bash\nif true;then\n echo ok\nfi\n",
                "driver.c": "int main(void){return 0;}\n",
            }
            paths: list[Path] = []
            for relative, text in contents.items():
                path = root / relative
                path.write_text(text, encoding="utf-8")
                paths.append(path)
            groups = classify_source_formats(paths, root=root)

            for name, command in formatter_commands(groups, workspace=str(root)):
                result = run_process(
                    command,
                    name=f"pinned {name} formatter",
                    timeout=30,
                    cwd=root,
                )
                self.assertEqual(result.returncode, 0, result.stderr)

            self.assertEqual((root / "tool.py").read_text(encoding="utf-8"), "value = 1\n")
            self.assertEqual(
                (root / "README.md").read_text(encoding="utf-8"),
                "# Demo\n\n- item\n",
            )
            self.assertEqual(
                (root / "data.json").read_text(encoding="utf-8"),
                '{ "value": 1 }\n',
            )
            self.assertEqual(
                (root / "target.toml").read_text(encoding="utf-8"),
                'name = "demo"\n',
            )
            shell = "#!/bin/sh\nif true; then\n\techo ok\nfi\n"
            self.assertEqual((root / "script.sh").read_text(encoding="utf-8"), shell)
            self.assertEqual(
                (root / "helper").read_text(encoding="utf-8"),
                shell.replace("#!/bin/sh", "#!/usr/bin/env bash"),
            )
            self.assertEqual(
                (root / "driver.c").read_text(encoding="utf-8"),
                "int main(void)\n{\n\treturn 0;\n}\n",
            )


if __name__ == "__main__":
    unittest.main()
