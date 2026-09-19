# SPDX-License-Identifier: GPL-2.0-only
"""Actual pinned-tool coverage for mixed project source formatting."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import check as source_check
from fplinux_cli.common import ROOT
from fplinux_cli.format import formatter_commands, resolve_format_paths
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
                "package-lock.json": '{"untouched":1}\n',
                "commitlint.config.mjs": "export default {value:1};\n",
                "alpine/aports/fplinux-micropythonos/fplinux-keypad-test.MANIFEST.JSON": (
                    '{"name":"Keypad"}\n'
                ),
                "alpine/abuild.conf": 'CFLAGS="-Os"\nCXXFLAGS="$CFLAGS"\n',
                "alpine/aports/fplinux-micropythonos-storage/micropythonos.conf": (
                    "if true;then\n MPOS_STORAGE=/mnt/card\nfi\n"
                ),
                "target.toml": 'name="demo"\n',
                "script.sh": "#!/bin/sh\nif true;then\n echo ok\nfi\n",
                "helper": "#!/usr/bin/env bash\nif true;then\n echo ok\nfi\n",
                "driver.c": "int main(void){return 0;}\n",
            }
            paths: list[Path] = []
            for relative, text in contents.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
                paths.append(path)
            selected = [path for path in contents if path != "package-lock.json"]
            _selected, _files, groups = resolve_format_paths(
                selected,
                root=root,
                inventory=[(path.relative_to(root).as_posix(), path) for path in paths],
            )

            for name, command in formatter_commands(groups, workspace=str(root)):
                result = run_process(
                    command,
                    name=f"pinned {name} formatter",
                    timeout=30,
                    cwd=root,
                )
                self.assertEqual(result.returncode, 0, result.stderr)

            with mock.patch.object(source_check, "ROOT", root):
                source_check.check_prettier_sources(groups, ("metadata", "docs"))
                source_check.check_shell_sources(groups)

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
                (root / "package-lock.json").read_text(encoding="utf-8"),
                '{"untouched":1}\n',
            )
            self.assertEqual(
                (root / "commitlint.config.mjs").read_text(encoding="utf-8"),
                "export default { value: 1 };\n",
            )
            self.assertEqual(
                (
                    root / "alpine/aports/fplinux-micropythonos/fplinux-keypad-test.MANIFEST.JSON"
                ).read_text(encoding="utf-8"),
                '{ "name": "Keypad" }\n',
            )
            self.assertEqual(
                (root / "alpine/abuild.conf").read_text(encoding="utf-8"),
                'CFLAGS="-Os"\nCXXFLAGS="$CFLAGS"\n',
            )
            self.assertEqual(
                (
                    root / "alpine/aports/fplinux-micropythonos-storage/micropythonos.conf"
                ).read_text(encoding="utf-8"),
                "if true; then\n\tMPOS_STORAGE=/mnt/card\nfi\n",
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

    def test_metadata_checker_rejects_invalid_javascript_and_uppercase_json(self) -> None:
        """Neither explicitly supported metadata format may be silently ignored."""
        cases = {
            "commitlint.config.mjs": "export default {;\n",
            "alpine/aports/fplinux-micropythonos/fplinux-keypad-test.MANIFEST.JSON": (
                "{invalid\n"
            ),
        }
        for relative, contents in cases.items():
            with self.subTest(path=relative), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents, encoding="utf-8")
                groups = classify_source_formats([path], root=root)

                with (
                    mock.patch.object(source_check, "ROOT", root),
                    self.assertRaises(subprocess.CalledProcessError) as failure,
                ):
                    source_check.check_prettier_sources(groups, ("metadata",))
                self.assertEqual(failure.exception.cmd[0], "prettier")

    def test_shell_checker_preserves_dialect_and_script_unused_variable_checks(self) -> None:
        """Sourced settings may export assignments but may not use Bash syntax."""
        cases = {
            "alpine/abuild.conf": "source /dev/null\n",
            "alpine/aports/fplinux-micropythonos-storage/micropythonos.conf": (
                "source /dev/null\n"
            ),
            "script.sh": "#!/bin/sh\nunused=1\n",
        }
        for relative, contents in cases.items():
            with self.subTest(path=relative), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents, encoding="utf-8")
                groups = classify_source_formats([path], root=root)

                with (
                    mock.patch.object(source_check, "ROOT", root),
                    self.assertRaises(subprocess.CalledProcessError) as failure,
                ):
                    source_check.check_shell_sources(groups)
                self.assertEqual(failure.exception.cmd[0], "shellcheck")


if __name__ == "__main__":
    unittest.main()
