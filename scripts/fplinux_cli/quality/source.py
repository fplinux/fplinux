#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fast, read-only source quality gate."""

from __future__ import annotations

import argparse
import re
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.alpine import lock as alpine_lock
from fplinux_cli.alpine import selection
from fplinux_cli.alpine.aports import materialize_aport_sources
from fplinux_cli.common import ROOT, fail
from fplinux_cli.data_paths import data_root
from fplinux_cli.manifests.paths import discover_targets
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.quality import markdown_links, source_commands, userspace_c
from fplinux_cli.quality.formatting.canonical import (
    canonical_outputs,
    formatter_commands,
    noncanonical_paths,
)
from fplinux_cli.quality.formatting.source_formats import classify_source_formats
from fplinux_cli.quality.scopes import SOURCE_CHECK_SCOPES
from fplinux_cli.reporting.run import RunReporter, run_entrypoint
from fplinux_cli.source_paths import source_path_excluded
from fplinux_cli.workspace.capture import workspace_snapshot

if TYPE_CHECKING:
    from collections.abc import Iterator

    from fplinux_cli.quality.formatting.source_formats import SourceFormats

EXCLUDED_PARTS = {".cache", ".git", "__pycache__", "node_modules"}
BINARY_SUFFIXES = {".bin", ".ico", ".jpg", ".png", ".pyc", ".ttf", ".zip"}
QUAKE_DATA_NAME = re.compile(r"pak[0-9]+\.part\.[0-9]+", re.IGNORECASE)
APORT_ROOT = ("alpine", "aports")


@contextmanager
def report_stage(reporter: RunReporter | None, name: str) -> Iterator[None]:
    """Use a persistent stage log when the host supplied one."""
    if reporter is None:
        yield
        return
    with reporter.stage(name):
        yield


def _source_tree(directory: Path, selected_data_root: Path) -> Iterator[Path]:
    """Prune generated and private trees before inspecting their contents."""
    for path in sorted(directory.iterdir()):
        relative = path.relative_to(ROOT)
        if source_path_excluded(path, source_root=ROOT, selected_data_root=selected_data_root):
            continue
        if any(part in EXCLUDED_PARTS for part in relative.parts):
            continue
        if relative.parts[:2] in {("site", ".astro"), ("site", "dist")}:
            continue
        if path.name == ".fplinux-workspace":
            continue
        yield path
        if not path.is_symlink() and path.is_dir():
            yield from _source_tree(path, selected_data_root)


def source_files(*, enforce_policy: bool) -> list[Path]:
    selected_data_root = data_root(ROOT)
    files: list[Path] = []
    for path in sorted(_source_tree(ROOT, selected_data_root)):
        relative = path.relative_to(ROOT)
        if path.is_symlink():
            if enforce_policy:
                fail(f"source symlink is not allowed: {relative}")
            continue
        if not path.is_file():
            continue
        application_image = (
            relative.parent == Path("alpine/aports/fplinux-example-apidemo")
            and relative.suffix == ".png"
        )
        if (
            is_site_asset(relative)
            or application_image
            or relative.as_posix() in {"site/src/fonts/OFL.txt", "site/public/fonts/OFL.txt"}
        ):
            continue
        if path.suffix.lower() == ".pak" or QUAKE_DATA_NAME.fullmatch(path.name):
            if enforce_policy:
                fail(f"Quake game data is not allowed in source: {relative}")
            continue
        if path.suffix in BINARY_SUFFIXES:
            if enforce_policy:
                fail(f"binary artifact is not allowed in source: {relative}")
            continue
        files.append(path)
    return files


def is_site_asset(relative: Path) -> bool:
    """Recognize bundled website images and fonts outside the text-source contract."""
    return (
        (relative.parts[:2] == ("site", "public") and relative.suffix in {".ico", ".png"})
        or (
            relative.parts[:3] == ("site", "src", "assets") and relative.suffix in {".jpg", ".png"}
        )
        or (relative.parts[:3] == ("site", "src", "fonts") and relative.suffix == ".ttf")
    )


def check_text(files: list[Path]) -> None:
    for path in files:
        relative = path.relative_to(ROOT)
        data = path.read_bytes()
        if b"\0" in data:
            fail(f"NUL byte in source file: {relative}")
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError as error:
            fail(f"source is not UTF-8: {relative}: {error}")
        if "\r" in text:
            fail(f"non-LF line ending: {relative}")
        if text and not text.endswith("\n"):
            fail(f"missing final newline: {relative}")
        for number, line in enumerate(text.splitlines(), 1):
            # A single space marks an empty unified-diff context line.
            if line.endswith((" ", "\t")) and not (path.suffix == ".patch" and line == " "):
                fail(f"trailing whitespace: {relative}:{number}")
            if path.suffix != ".patch" and re.search(r" +\t", line):
                fail(f"space before tab: {relative}:{number}")


def alpine_apkbuilds(files: list[Path]) -> list[str]:
    """Discover regular first-party Alpine recipes."""
    result = [
        path.relative_to(ROOT).as_posix()
        for path in files
        if (relative := path.relative_to(ROOT)).parts[:2] == APORT_ROOT
        and len(relative.parts) == 4
        and relative.name == "APKBUILD"
    ]
    if not result:
        fail("no Alpine APKBUILD files were discovered")
    return result


def validate_package_selections() -> None:
    """Require every target's declared package set to resolve to current aports."""
    for target in discover_targets():
        target_config = load_target(target)
        platform = load_platform(target_config["platform"])
        rootfs_packages = selection.selected_packages(platform, target_config, root=ROOT)
        selection.bundle_packages(platform, target_config, rootfs_packages, root=ROOT)


def check_canonical_sources(
    files: list[Path], formats: SourceFormats, scopes: tuple[str, ...]
) -> None:
    """Compare the formatter's expected bytes without writing the source checkout."""
    selected: set[str] = set()
    for scope, paths in (
        ("source", (*formats.devicetree, *formats.text)),
        ("metadata", (*formats.toml, *formats.json, *formats.yaml, *formats.javascript)),
        ("docs", formats.markdown),
        ("python", formats.python),
        ("shell", (*formats.posix_shell, *formats.bash, *formats.posix_shell_fragments)),
        ("alpine", tuple(path for path in formats.text if Path(path).name == "APKBUILD")),
    ):
        if scope in scopes:
            selected.update(paths)
    if "metadata" in scopes:
        selected.update(
            path
            for path in formats.text
            if Path(path).suffix == ".ini" or Path(path).name == ".editorconfig"
        )
    if "c" in scopes:
        selected.update(userspace_c.project_c_format_sources(files))
        selected.update(path for path in formats.c if "bootstrap" in Path(path).parts)
    if not selected:
        return
    paths = tuple(sorted(selected))
    groups = formats.select(frozenset(selected))
    snapshot = workspace_snapshot([(path.relative_to(ROOT).as_posix(), path) for path in files])

    def run_formatters(projection: Path) -> None:
        for _name, command in formatter_commands(groups, workspace=str(projection)):
            source_commands.run(command, cwd=projection)

    outputs = canonical_outputs(snapshot, paths, run_formatters=run_formatters)
    changed = noncanonical_paths(snapshot, outputs)
    if changed:
        fail("source formatting differs: " + ", ".join(changed))


def check_shell_sources(formats: SourceFormats) -> None:
    """Check declared shell dialects, including configurations sourced by consumers."""
    scripts = (*formats.posix_shell, *formats.bash)
    if scripts:
        source_commands.run(
            ["shellcheck", "--enable=all", "--severity=warning", *scripts], cwd=ROOT
        )
    if formats.posix_shell_fragments:
        # These assignments are consumed by the sourcing program, not this file.
        source_commands.run(
            [
                "shellcheck",
                "--enable=all",
                "--severity=warning",
                "--shell=sh",
                "--exclude=SC2034",
                *formats.posix_shell_fragments,
            ],
            cwd=ROOT,
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("scopes", nargs="*", choices=SOURCE_CHECK_SCOPES)
    args = parser.parse_args()
    selected = tuple(dict.fromkeys(args.scopes)) if args.scopes else SOURCE_CHECK_SCOPES
    reporter = RunReporter.from_environment("check", "quality")

    with report_stage(reporter, "source-inventory"):
        files = source_files(enforce_policy="source" in selected)
        formats = classify_source_formats(files, root=ROOT)
        python_files = list(formats.python)
        markdown_files = list(formats.markdown)
        toml_files = list(formats.toml)

    if "source" in selected:
        with report_stage(reporter, "source-text"):
            check_text(files)
    with report_stage(reporter, "canonical-sources"):
        check_canonical_sources(files, formats, selected)
    if "metadata" in selected:
        with report_stage(reporter, "metadata"):
            source_commands.run(["taplo", "check", *toml_files])
    if "docs" in selected:
        markdown_paths = [
            ROOT / relative for relative in markdown_files if Path(relative).suffix == ".md"
        ]
        text_files = [
            str(path.relative_to(ROOT))
            for path in files
            if path.suffix == ".txt" and path.relative_to(ROOT).parts[0] != "LICENSES"
        ]
        with report_stage(reporter, "documentation"):
            markdown_links.check_markdown_links(markdown_paths)
            source_commands.run(["markdownlint-cli2"])
            source_commands.run(["vale", "--config", ".vale.ini", *markdown_files, *text_files])
    if "spelling" in selected:
        with report_stage(reporter, "spelling"):
            source_commands.run(["typos", "."])
    if "secrets" in selected:
        with report_stage(reporter, "secrets"):
            source_commands.run(
                [
                    "gitleaks",
                    "detect",
                    "--no-banner",
                    "--no-git",
                    "--source",
                    ".",
                    "--redact",
                    "--exit-code",
                    "1",
                ]
            )
    if "licenses" in selected:
        with report_stage(reporter, "licenses"):
            source_commands.run(["python3", "-B", "-m", "reuse", "lint"])
    if "python" in selected:
        with report_stage(reporter, "python"):
            source_commands.run(["ruff", "check", *python_files])
            source_commands.run(["python3", "-B", "-m", "mypy", *python_files])
    if "shell" in selected:
        with report_stage(reporter, "shell"):
            check_shell_sources(formats)
    if "alpine" in selected:
        apkbuilds = alpine_apkbuilds(files)
        with report_stage(reporter, "alpine"):
            alpine_lock.load_alpine_lock()
            validate_package_selections()
            source_commands.run(["sh", "-n", "alpine/abuild.conf"])
            for apkbuild in apkbuilds:
                package = Path(apkbuild).parent.name
                with tempfile.TemporaryDirectory(prefix="fplinux-aport-lint-") as temporary:
                    staged = materialize_aport_sources(package, ROOT, Path(temporary) / package)
                    source_commands.run(["apkbuild-lint", "APKBUILD"], cwd=staged)
    if "c" in selected:
        with report_stage(reporter, "ui-profile"):
            source_commands.run(["python3", "scripts/fplinux_ui_profile.py", "--check"])
        c_sources = userspace_c.userspace_c_sources(files)
        with report_stage(reporter, "c-analysis"):
            userspace_c.check_userspace_c(c_sources)

    inventory = (
        f"{len(files)} UTF-8 source files"
        if "source" in selected
        else f"{len(files)} source files inventoried"
    )
    print(f"source checks: OK ({inventory}; scopes: {','.join(selected)})")


if __name__ == "__main__":
    run_entrypoint(main)
