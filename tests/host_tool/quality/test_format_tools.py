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

import pytest
from fplinux_cli.cli import target_new
from fplinux_cli.common import ROOT
from fplinux_cli.manifests.identity import validate_target_identity
from fplinux_cli.quality import source as source_check
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


FIXTURES = ROOT / "tests/fixtures/quality/host-tool"


class PinnedFormatterToolTests:
    """Run every existing formatter against one private mixed projection."""

    def test_toml_check_rejects_noncanonical_spacing_in_its_projection(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """Taplo must check projected inputs without changing the original document."""
        shutil.copyfile(ROOT / ".taplo.toml", tmp_path / ".taplo.toml")
        source = tmp_path / "target.toml"
        original = (FIXTURES / "mixed-format/input/target.toml.fixture").read_bytes()
        source.write_bytes(original)
        files = [tmp_path / ".taplo.toml", source]
        groups = classify_source_formats(files, root=tmp_path)
        monkeypatch.setattr(source_check, "ROOT", tmp_path)

        with pytest.raises(SystemExit, match=r"source formatting differs: target\.toml"):
            source_check.check_canonical_sources(files, groups, ("metadata",))

        assert source.read_bytes() == original
        source.write_bytes((FIXTURES / "mixed-format/expected/target.toml.fixture").read_bytes())
        source_check.check_canonical_sources(files, groups, ("metadata",))

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
            inputs = FIXTURES / "mixed-format/input"
            paths: list[Path] = []
            for fixture in sorted(inputs.rglob("*.fixture")):
                relative = fixture.relative_to(inputs).as_posix().removesuffix(".fixture")
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(fixture.read_bytes())
                paths.append(path)
            selected = [
                path.relative_to(root).as_posix()
                for path in paths
                if path.name != "package-lock.json"
            ]
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

            expectations = FIXTURES / "mixed-format/expected"
            expected_files = sorted(expectations.rglob("*.fixture"))
            assert paths
            assert {path.relative_to(root).as_posix() for path in paths} == {
                fixture.relative_to(expectations).as_posix().removesuffix(".fixture")
                for fixture in expected_files
            }
            for fixture in expected_files:
                relative = fixture.relative_to(expectations).as_posix().removesuffix(".fixture")
                assert (root / relative).read_bytes() == fixture.read_bytes()

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

    @pytest.mark.parametrize("scenario", ["headings", "table"], ids=["headings", "table"])
    def test_markdown_formatting_preserves_heading_and_table_order(self, scenario: str) -> None:
        """Formatting is idempotent without reordering meaningful document contents."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "targets/phone/README.md"
            path.parent.mkdir(parents=True)
            case = FIXTURES / "markdown" / scenario
            path.write_bytes((case / "input.md.fixture").read_bytes())
            groups = classify_source_formats([path], root=root)
            results: list[bytes] = []
            for _pass in range(2):
                for name, command in formatter_commands(groups, workspace=str(root)):
                    result = run_process(command, name=f"Markdown {name}", timeout=30, cwd=root)
                    assert (result.returncode) == (0), result.stderr
                results.append(path.read_bytes())
            assert (results[0]) == (results[1])
            if scenario == "headings":
                assert results[0] == (case / "expected.md.fixture").read_bytes()
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
        self, relative: str, contents: str, capfd: pytest.CaptureFixture[str]
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
                pytest.raises(subprocess.CalledProcessError),
            ):
                source_check.check_canonical_sources([path], groups, ("metadata",))
            captured = capfd.readouterr()
            assert "SyntaxError" in captured.err
            assert relative in captured.err

    @pytest.mark.parametrize(
        ("relative", "contents", "diagnostic"),
        [
            ("alpine/abuild.conf", "source /dev/null\n", "SC3046"),
            ("script.sh", "#!/bin/sh\nunused=1\n", "SC2034"),
            ("script.sh.in", "#!/bin/sh\nunused='@VALUE@'\n", "SC2034"),
        ],
        ids=["posix-fragment-bash-source", "script-unused-variable", "template-unused-variable"],
    )
    def test_shell_checker_preserves_dialect_and_script_unused_variable_checks(
        self, relative: str, contents: str, diagnostic: str, capfd: pytest.CaptureFixture[str]
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
                pytest.raises(subprocess.CalledProcessError),
            ):
                source_check.check_shell_sources(groups)
            captured = capfd.readouterr()
            assert diagnostic in captured.out
            assert relative in captured.out
