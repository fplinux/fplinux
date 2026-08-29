# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for the shared source formatter classification."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli.source_formats import classify_source_formats


class SourceFormatClassificationTests(unittest.TestCase):
    """Keep check and format on one observable file-to-tool contract."""

    def test_current_formatter_boundaries_are_classified_once(self) -> None:
        """Route supported source types and leave checker-only files unsupported."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            contents = {
                "tool.py": "value=1\n",
                "README.md": "# Demo\n",
                "data.json": "{}\n",
                "settings.jsonc": "{}\n",
                "package-lock.json": "{}\n",
                "target.toml": "name='demo'\n",
                "driver.c": "int value;\n",
                "driver.h": "int value;\n",
                "script.sh": "#!/bin/sh\necho ok\n",
                "service.initd": "#!/sbin/openrc-run\n",
                "helper": "#!/usr/bin/env bash\necho ok\n",
                "binding.yaml": "---\n",
                "notes.txt": "plain\n",
            }
            for relative, text in contents.items():
                (root / relative).write_text(text, encoding="utf-8")

            formats = classify_source_formats(
                [root / relative for relative in contents],
                root=root,
            )

        self.assertEqual(formats.python, ("tool.py",))
        self.assertEqual(formats.markdown, ("README.md",))
        self.assertEqual(formats.json, ("data.json", "settings.jsonc"))
        self.assertEqual(formats.toml, ("target.toml",))
        self.assertEqual(formats.posix_shell, ("script.sh", "service.initd"))
        self.assertEqual(formats.bash, ("helper",))
        self.assertEqual(formats.c, ("driver.c", "driver.h"))
        self.assertNotIn("package-lock.json", formats.supported())
        self.assertNotIn("binding.yaml", formats.supported())
        self.assertNotIn("notes.txt", formats.supported())


if __name__ == "__main__":
    unittest.main()
