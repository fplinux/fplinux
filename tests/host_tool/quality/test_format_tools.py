# SPDX-License-Identifier: GPL-2.0-only
"""Actual pinned-tool coverage for mixed project source formatting."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import check as source_check
from fplinux_cli.cli import target_new
from fplinux_cli.common import ROOT
from fplinux_cli.manifests.identity import validate_target_identity
from fplinux_cli.quality.formatting.canonical import (
    canonical_outputs,
    formatter_commands,
    noncanonical_paths,
)
from fplinux_cli.quality.formatting.command import resolve_format_paths
from fplinux_cli.quality.formatting.source_formats import classify_source_formats
from fplinux_cli.workspace.capture import workspace_snapshot

from tests.process import run_process

if TYPE_CHECKING:
    from fplinux_cli.quality.formatting.source_formats import SourceFormats


class PinnedFormatterToolTests(unittest.TestCase):
    """Run every existing formatter against one private mixed projection."""

    def test_pinned_tools_format_their_current_source_boundaries(self) -> None:
        """Each routed source reaches its real tool and obtains canonical bytes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in (
                ".clang-format",
                ".editorconfig",
                ".prettierrc.json",
                ".taplo.toml",
                "pyproject.toml",
            ):
                shutil.copy2(ROOT / name, root / name)
            contents = {
                "tool.py": "value=  1\n",
                "README.md": "# Demo\n\n-   item\n",
                "data.json": '{"value":1}\n',
                "package-lock.json": '{"untouched":1}\n',
                "commitlint.config.mjs": "export default {value:1};\n",
                "alpine/abuild.conf": 'CFLAGS="-Os"\nCXXFLAGS="$CFLAGS"\n',
                "target.toml": 'name="demo"\n',
                "target.toml.in": 'platform="@PLATFORM@"\n',
                "README.md.in": "# @DEVICE@\n\n-   item\n",
                ".github/workflows/demo.yml": "jobs: {}\nname: Demo\non: [push]\n",
                "phone.dts.in": (
                    '/dts-v1/;\n/ {\n\tstatus = "okay";\n\tcompatible = "@COMPATIBLE@";\n};\n'
                ),
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
                check_files = [path for path in root.rglob("*") if path.is_file()]
                source_check.check_canonical_sources(check_files, groups, ("metadata", "docs"))
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
                (root / "alpine/abuild.conf").read_text(encoding="utf-8"),
                'CFLAGS="-Os"\nCXXFLAGS="$CFLAGS"\n',
            )
            self.assertEqual(
                (root / "target.toml").read_text(encoding="utf-8"),
                'name = "demo"\n',
            )
            self.assertEqual(
                (root / "target.toml.in").read_text(encoding="utf-8"), 'platform = "@PLATFORM@"\n'
            )
            self.assertEqual(
                (root / "README.md.in").read_text(encoding="utf-8"), "# @DEVICE@\n\n- item\n"
            )
            self.assertEqual(
                (root / ".github/workflows/demo.yml").read_text(encoding="utf-8"),
                "name: Demo\non: [push]\njobs: {}\n",
            )
            self.assertEqual(
                (root / "phone.dts.in").read_text(encoding="utf-8"),
                '/dts-v1/;\n/ {\n\tcompatible = "@COMPATIBLE@";\n\n\tstatus = "okay";\n};\n',
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

    def test_canonical_templates_render_without_a_second_formatting_change(self) -> None:
        """Real rendered files retain canonical bytes after placeholder substitution."""
        provider = target_new._skeleton_provider("ums9117")  # noqa: SLF001 -- template boundary.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in (
                ".clang-format",
                ".editorconfig",
                ".prettierrc.json",
                ".taplo.toml",
                "pyproject.toml",
            ):
                shutil.copy2(ROOT / name, root / name)
            template = root / "platforms/ums9117/target-template"
            shutil.copytree(provider.template_directory(), template)
            paths = [path for path in root.rglob("*") if path.is_file()]
            groups = classify_source_formats(paths, root=root)
            for name, command in formatter_commands(groups, workspace=str(root)):
                result = run_process(command, name=f"template {name}", timeout=30, cwd=root)
                self.assertEqual(result.returncode, 0, result.stderr)

            for target, product in (
                ("short", "P"),
                ("long-phone-name", "Horizon LTE Extra Long 2"),
            ):
                with self.subTest(target=target):
                    identity = validate_target_identity(
                        {
                            "brand": "HAMMER",
                            "product": product,
                            "hardware_codes": [],
                            "compatible": "hammer,phone",
                        }
                    )
                    values = provider.template_values(target, identity)
                    rendered = target_new._render_template(template, values)  # noqa: SLF001 -- rendered artifact boundary.
                    destination = root / "targets" / target
                    for relative, contents in rendered.items():
                        path = destination / relative
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(contents)
                        self.assertNotRegex(contents.decode("utf-8"), r"@[A-Z_]+@")
                    inventory = [
                        (path.relative_to(root).as_posix(), path)
                        for path in root.rglob("*")
                        if path.is_file()
                    ]
                    selected = tuple(
                        (destination / relative).relative_to(root).as_posix()
                        for relative in rendered
                    )
                    formats = classify_source_formats(
                        [path for _relative, path in inventory], root=root
                    )
                    groups = formats.select(frozenset(selected))
                    snapshot = workspace_snapshot(inventory)

                    def run_formatters(
                        projection: Path, *, groups: SourceFormats = groups
                    ) -> None:
                        for name, command in formatter_commands(groups, workspace=str(projection)):
                            result = run_process(
                                command, name=f"rendered {name}", timeout=30, cwd=projection
                            )
                            self.assertEqual(result.returncode, 0, result.stderr)

                    outputs = canonical_outputs(snapshot, selected, run_formatters=run_formatters)
                    self.assertEqual(noncanonical_paths(snapshot, outputs), ())

    def test_markdown_structure_is_canonical_after_one_tool_pipeline(self) -> None:
        """Equivalent heading and table spellings need no later ordering pass."""
        cases = (
            (
                "headings",
                (
                    b"# Phone\n\n## Features\n\nFeature text.\n\n## Identity ##\n\n"
                    b"Identity text.\n\n## Status\n\nStatus text.\n"
                ),
            ),
            (
                "table",
                (
                    b"# Phone\n\n## Features\n\nFeature | Hardware | FPLinux | This phone\n"
                    b"--- | --- | --- | ---\nCamera | Present | Partial | Rear sensor.\n"
                    b"USB networking | Present | Supported | USB only.\n"
                ),
            ),
        )
        for scenario, contents in cases:
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path = root / "targets/phone/README.md"
                path.parent.mkdir(parents=True)
                path.write_bytes(contents)
                groups = classify_source_formats([path], root=root)
                results: list[bytes] = []
                for _pass in range(2):
                    for name, command in formatter_commands(groups, workspace=str(root)):
                        result = run_process(
                            command, name=f"Markdown {name}", timeout=30, cwd=root
                        )
                        self.assertEqual(result.returncode, 0, result.stderr)
                    results.append(path.read_bytes())
                self.assertEqual(results[0], results[1])
                if scenario == "headings":
                    self.assertEqual(
                        results[0],
                        b"# Phone\n\n## Identity\n\nIdentity text.\n\n"
                        b"## Status\n\nStatus text.\n\n## Features\n\nFeature text.\n",
                    )
                else:
                    rows = [
                        [cell.strip() for cell in line.strip("|").split("|")]
                        for line in results[0].decode().splitlines()
                        if line.startswith("|")
                    ]
                    self.assertEqual(
                        rows[2:],
                        [
                            ["USB networking", "Present", "Supported", "USB only."],
                            ["Camera", "Present", "Partial", "Rear sensor."],
                        ],
                    )

    def test_metadata_checker_rejects_invalid_javascript(self) -> None:
        """Explicitly supported JavaScript metadata may not be silently ignored."""
        cases = {
            "commitlint.config.mjs": "export default {;\n",
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
                    source_check.check_canonical_sources([path], groups, ("metadata",))
                self.assertEqual(failure.exception.cmd[0], "prettier")

    def test_shell_checker_preserves_dialect_and_script_unused_variable_checks(self) -> None:
        """Sourced settings may export assignments but may not use Bash syntax."""
        cases = {
            "alpine/abuild.conf": "source /dev/null\n",
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
