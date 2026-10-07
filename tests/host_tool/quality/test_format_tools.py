# SPDX-License-Identifier: GPL-2.0-only
"""Actual pinned-tool coverage for mixed project source formatting."""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import check as source_check
import pytest
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


class PinnedFormatterToolTests:
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
                assert (result.returncode) == (0), result.stderr

            with mock.patch.object(source_check, "ROOT", root):
                check_files = [path for path in root.rglob("*") if path.is_file()]
                source_check.check_canonical_sources(check_files, groups, ("metadata", "docs"))
                source_check.check_shell_sources(groups)

            assert ((root / "tool.py").read_text(encoding="utf-8")) == ("value = 1\n")
            assert ((root / "README.md").read_text(encoding="utf-8")) == ("# Demo\n\n- item\n")
            assert ((root / "data.json").read_text(encoding="utf-8")) == ('{ "value": 1 }\n')
            assert ((root / "package-lock.json").read_text(encoding="utf-8")) == (
                '{"untouched":1}\n'
            )
            assert ((root / "commitlint.config.mjs").read_text(encoding="utf-8")) == (
                "export default { value: 1 };\n"
            )
            assert ((root / "alpine/abuild.conf").read_text(encoding="utf-8")) == (
                'CFLAGS="-Os"\nCXXFLAGS="$CFLAGS"\n'
            )
            assert ((root / "target.toml").read_text(encoding="utf-8")) == ('name = "demo"\n')
            assert ((root / "target.toml.in").read_text(encoding="utf-8")) == (
                'platform = "@PLATFORM@"\n'
            )
            assert ((root / "README.md.in").read_text(encoding="utf-8")) == (
                "# @DEVICE@\n\n- item\n"
            )
            assert ((root / ".github/workflows/demo.yml").read_text(encoding="utf-8")) == (
                "name: Demo\non: [push]\njobs: {}\n"
            )
            assert ((root / "phone.dts.in").read_text(encoding="utf-8")) == (
                '/dts-v1/;\n/ {\n\tcompatible = "@COMPATIBLE@";\n\n\tstatus = "okay";\n};\n'
            )
            shell = "#!/bin/sh\nif true; then\n\techo ok\nfi\n"
            assert ((root / "script.sh").read_text(encoding="utf-8")) == (shell)
            assert ((root / "helper").read_text(encoding="utf-8")) == (
                shell.replace("#!/bin/sh", "#!/usr/bin/env bash")
            )
            assert ((root / "driver.c").read_text(encoding="utf-8")) == (
                "int main(void)\n{\n\treturn 0;\n}\n"
            )

    @pytest.mark.parametrize(
        ("target", "product"),
        [("short", "P"), ("long-phone-name", "Horizon LTE Extra Long 2")],
        ids=["short-P", "long-phone-name-Horizon-LTE-Extra-Long-2"],
    )
    def test_canonical_templates_render_without_a_second_formatting_change(
        self, target: str, product: str
    ) -> None:
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
                assert (result.returncode) == (0), result.stderr

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
                assert re.search(r"@[A-Z_]+@", contents.decode("utf-8")) is None
            inventory = [
                (path.relative_to(root).as_posix(), path)
                for path in root.rglob("*")
                if path.is_file()
            ]
            selected = tuple(
                (destination / relative).relative_to(root).as_posix() for relative in rendered
            )
            formats = classify_source_formats([path for _relative, path in inventory], root=root)
            groups = formats.select(frozenset(selected))
            snapshot = workspace_snapshot(inventory)

            def run_formatters(projection: Path, *, groups: SourceFormats = groups) -> None:
                for name, command in formatter_commands(groups, workspace=str(projection)):
                    result = run_process(
                        command, name=f"rendered {name}", timeout=30, cwd=projection
                    )
                    assert (result.returncode) == (0), result.stderr

            outputs = canonical_outputs(snapshot, selected, run_formatters=run_formatters)
            assert (noncanonical_paths(snapshot, outputs)) == (())

    @pytest.mark.parametrize(
        ("scenario", "contents"),
        [
            (
                "headings",
                (
                    b"# Phone\n\n## Features\n\nFeature text.\n\n"
                    b"## Identity ##\n\nIdentity text.\n\n## Status\n\nStatus text.\n"
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
        ],
        ids=[
            "headings-bytes-232050686f6e650a0a23232046656174757265730a0a4665617475726520746578742e0a0a2323204964656e746974792023230a0",
            "table-bytes-232050686f6e650a0a23232046656174757265730a0a46656174757265207c204861726477617265207c2046504c696e7578207c2054",
        ],
    )
    def test_markdown_formatting_preserves_heading_and_table_order(
        self, scenario: str, contents: bytes
    ) -> None:
        """Formatting is idempotent without reordering meaningful document contents."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "targets/phone/README.md"
            path.parent.mkdir(parents=True)
            path.write_bytes(contents)
            groups = classify_source_formats([path], root=root)
            results: list[bytes] = []
            for _pass in range(2):
                for name, command in formatter_commands(groups, workspace=str(root)):
                    result = run_process(command, name=f"Markdown {name}", timeout=30, cwd=root)
                    assert (result.returncode) == (0), result.stderr
                results.append(path.read_bytes())
            assert (results[0]) == (results[1])
            if scenario == "headings":
                assert (results[0]) == (
                    b"# Phone\n\n## Features\n\nFeature text.\n\n"
                    b"## Identity\n\nIdentity text.\n\n## Status\n\nStatus text.\n"
                )
            else:
                rows = [
                    [cell.strip() for cell in line.strip("|").split("|")]
                    for line in results[0].decode().splitlines()
                    if line.startswith("|")
                ]
                assert (rows[2:]) == (
                    [
                        ["Camera", "Present", "Partial", "Rear sensor."],
                        ["USB networking", "Present", "Supported", "USB only."],
                    ]
                )

    @pytest.mark.parametrize(
        ("relative", "contents"),
        [
            ("commitlint.config.mjs", "export default {;\n"),
        ],
        ids=["commitlint.config.mjs-export-default-n"],
    )
    def test_metadata_checker_rejects_invalid_javascript(
        self, relative: str, contents: str
    ) -> None:
        """Explicitly supported JavaScript metadata may not be silently ignored."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
            groups = classify_source_formats([path], root=root)

            with (
                mock.patch.object(source_check, "ROOT", root),
                pytest.raises(subprocess.CalledProcessError) as failure,
            ):
                source_check.check_canonical_sources([path], groups, ("metadata",))
            assert (failure.value.cmd[0]) == ("prettier")

    @pytest.mark.parametrize(
        ("relative", "contents"),
        [("alpine/abuild.conf", "source /dev/null\n"), ("script.sh", "#!/bin/sh\nunused=1\n")],
        ids=["alpine-abuild.conf-source-dev-null-n", "script.sh-bin-sh-nunused-1-n"],
    )
    def test_shell_checker_preserves_dialect_and_script_unused_variable_checks(
        self, relative: str, contents: str
    ) -> None:
        """Sourced settings may export assignments but may not use Bash syntax."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
            groups = classify_source_formats([path], root=root)

            with (
                mock.patch.object(source_check, "ROOT", root),
                pytest.raises(subprocess.CalledProcessError) as failure,
            ):
                source_check.check_shell_sources(groups)
            assert (failure.value.cmd[0]) == ("shellcheck")
