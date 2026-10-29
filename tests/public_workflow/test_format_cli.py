# SPDX-License-Identifier: GPL-2.0-only
"""Public command-line evidence for explicit source formatting."""

from __future__ import annotations

import shutil
from pathlib import Path
from typing import TYPE_CHECKING

from tests.cli_support import prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]


class FormatCliWorkflowTests:
    """Exercise public parser and pre-runtime refusal behavior."""

    def run_format(self, *arguments: str, root: Path = ROOT) -> subprocess.CompletedProcess[str]:
        """Invoke the repository entrypoint with a bounded process lifetime."""
        return run_process(
            [str(root / "fplinux"), "format", *arguments],
            name="public fplinux format",
            timeout=30,
            cwd=root,
        )

    def test_help_requires_explicit_repository_relative_paths(self) -> None:
        """Advertise a path-only mutating interface without recursive defaults."""
        result = self.run_format("--help")

        assert (result.returncode) == (0), result.stderr
        assert ("[PATH ...]") in (result.stdout)
        assert "--plan" in result.stdout
        assert "--changed" in result.stdout
        assert ("--all") not in (result.stdout)

    def test_missing_path_is_rejected_by_the_public_parser(self) -> None:
        """Formatting cannot silently expand to the complete checkout."""
        result = self.run_format()

        assert (result.returncode) != (0)
        assert ("format requires PATH or --changed") in (result.stderr)

    def test_plan_reports_mixed_ownership_and_invalid_paths_without_preparation(
        self, tmp_path: Path
    ) -> None:
        """Plan all paths without preparing a runtime or creating a data root."""
        checkout = tmp_path / "checkout"
        checkout.mkdir()
        prepare_cli_checkout(checkout)
        page = checkout / "site/src/content/docs/develop/code-style/index.mdx"
        page.parent.mkdir(parents=True)
        page.write_text("# Example\n")
        (checkout / "site/.prettierignore").write_text("package-lock.json\n")
        translation = checkout / "alpine/aports/fplinux-notes/ru.po"
        translation.parent.mkdir(parents=True)
        translation.write_text('msgid "hello"\nmsgstr "hello"\n')
        initialized = run_process(
            ["git", "init", "-q"], name="prepare Git inventory", timeout=20, cwd=checkout
        )
        assert initialized.returncode == 0, initialized.stderr
        result = self.run_format(
            "--plan",
            "scripts/fplinux_cli/cli/logs.py",
            "site/src/content/docs/develop/code-style/index.mdx",
            "alpine/aports/fplinux-notes/ru.po",
            "missing-file.po",
            root=checkout,
        )

        assert result.returncode == 0, result.stderr
        assert "format: scripts/fplinux_cli/cli/logs.py:" in result.stdout
        assert "site: site/src/content/docs/develop/code-style/index.mdx:" in result.stdout
        assert "unsupported: alpine/aports/fplinux-notes/ru.po:" in result.stdout
        assert "invalid: missing-file.po:" in result.stdout
        assert "./fplinux site format" in result.stdout
        assert not (checkout / ".data").exists()

    def test_changed_plan_handles_staged_rename_untracked_and_deletion(
        self, tmp_path: Path
    ) -> None:
        """Select current changed files, including renamed and untracked paths."""
        checkout = tmp_path / "checkout"
        checkout.mkdir()
        prepare_cli_checkout(checkout)
        samples = checkout / "samples"
        samples.mkdir()
        (samples / "modified.py").write_text("old = 1\n")
        (samples / "renamed.py").write_text("old = 2\n")
        (samples / "deleted.py").write_text("old = 3\n")
        (checkout / ".gitignore").write_text("samples/ignored.py\n")
        commands = (
            ("git", "init", "-q"),
            ("git", "add", ".gitignore", "samples"),
            (
                "git",
                "-c",
                "user.name=Fixture",
                "-c",
                "user.email=fixture@example.invalid",
                "commit",
                "-qm",
                "fixture",
            ),
            ("git", "mv", "samples/renamed.py", "samples/renamed file.py"),
            ("git", "rm", "-q", "samples/deleted.py"),
        )
        for command in commands:
            result = run_process(command, name="prepare Git selection", timeout=20, cwd=checkout)
            assert result.returncode == 0, result.stderr
        (checkout / ".git/info/exclude").write_text("/scripts/\n/fplinux\n/targets/\n")
        (samples / "modified.py").write_text("new = 1\n")
        (samples / "new.py").write_text("new = 2\n")
        (samples / "ignored.py").write_text("ignored = 1\n")

        result = self.run_format("--plan", "--changed", "samples", root=checkout)

        assert result.returncode == 0, result.stderr
        assert "format: samples/modified.py:" in result.stdout
        assert "format: samples/renamed file.py:" in result.stdout
        assert "format: samples/new.py:" in result.stdout
        assert "deleted.py" not in result.stdout
        assert "ignored.py" not in result.stdout
        assert not (checkout / ".data").exists()

        root_selection = self.run_format("--plan", "--changed", ".", root=checkout)
        assert root_selection.returncode == 0, root_selection.stderr
        assert "format: samples/modified.py:" in root_selection.stdout

    def test_plan_distinguishes_declared_and_undeclared_linux_patches(
        self, tmp_path: Path
    ) -> None:
        """Use declared integration contexts to route patches without preparing Linux."""
        checkout = tmp_path / "checkout"
        shutil.copytree(
            ROOT,
            checkout,
            ignore=shutil.ignore_patterns(".git", ".data", ".private", "__pycache__"),
        )
        undeclared = "platforms/ums9117/linux/patches/undeclared-test.patch"
        (checkout / undeclared).write_text("not an integration patch\n")
        initialized = run_process(
            ["git", "init", "-q"], name="prepare Git inventory", timeout=20, cwd=checkout
        )
        assert initialized.returncode == 0, initialized.stderr
        declared = "platforms/shared/linux/patches/0001-sprd-pmic.patch"

        result = self.run_format("--plan", declared, undeclared, root=checkout)

        assert result.returncode == 0, result.stderr
        assert f"format: {declared}: declared Linux patch" in result.stdout
        assert f"unsupported: {undeclared}: not a declared Linux patch" in result.stdout
        assert not (checkout / ".data").exists()

    def test_unsupported_source_is_rejected_before_runtime(self, tmp_path: Path) -> None:
        """A checker-only source gets a named public error without tool execution."""
        checkout = tmp_path / "source"
        shutil.copytree(
            ROOT,
            checkout,
            ignore=shutil.ignore_patterns(".git", ".data", "__pycache__"),
        )
        source = "targets/nokia-ta1618/release/EXTRA.txt"
        (checkout / source).write_text("Additional archive instructions.\n", encoding="utf-8")
        for command in (("git", "init", "-q"), ("git", "add", "--all")):
            prepared = run_process(
                command,
                name="temporary Git source inventory",
                timeout=30,
                cwd=checkout,
            )
            assert (prepared.returncode) == (0), prepared.stderr
        result = self.run_format(source, root=checkout)

        assert (result.returncode) != (0)
        assert ("no project formatter is defined") in (result.stderr)
        assert ("kern") not in (result.stdout.lower())
        assert ("kern") not in (result.stderr.lower())
