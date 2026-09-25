# SPDX-License-Identifier: GPL-2.0-only
"""Create a headless target skeleton from the platform target template."""

from __future__ import annotations

import importlib.util
import re
import shutil
from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.identity import IdentityError, validate_target_identity
from fplinux_cli.identity_codegen import validate_bootstrap_display_name
from fplinux_cli.manifests.paths import discover_platforms
from fplinux_cli.manifests.releases import load_release
from fplinux_cli.manifests.targets import load_target

if TYPE_CHECKING:
    from collections.abc import Mapping
    from pathlib import Path
    from types import ModuleType

# The platform-owned module that supplies the target template and its placeholder values.
SKELETON_MODULE = "host/target_skeleton.py"
# The lowercase, hyphen-separated form used by the existing target names.
NEW_TARGET_NAME = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*")
# A template file ending in this suffix has its placeholders substituted.
_TEMPLATE_SUFFIX = ".in"
_PLACEHOLDER = re.compile(r"@([A-Z][A-Z_]*)@")


def derived_compatible(brand: str, product: str) -> str:
    """Form a lowercase vendor,device compatible from the public names."""

    def part(text: str) -> str:
        return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")

    return f"{part(brand)},{part(product)}"


def _substitute(text: str, values: Mapping[str, str]) -> str:
    """Replace every @NAME@ placeholder, refusing names the template does not define."""

    def replacement(match: re.Match[str]) -> str:
        name = match.group(1)
        if name not in values:
            fail(f"target template uses an unknown placeholder: @{name}@")
        return values[name]

    return _PLACEHOLDER.sub(replacement, text)


def _align_markdown_tables(text: str) -> str:
    """Pad pipe tables as the Markdown formatter does, so the README passes its check.

    Cells are measured by character count; template tables hold only single-width text.
    """
    lines = text.split("\n")
    aligned: list[str] = []
    index = 0
    while index < len(lines):
        if not lines[index].startswith("|"):
            aligned.append(lines[index])
            index += 1
            continue
        end = index
        while end < len(lines) and lines[end].startswith("|"):
            end += 1
        rows = [
            [cell.strip() for cell in line.strip().strip("|").split("|")]
            for line in lines[index:end]
        ]
        content = [row for number, row in enumerate(rows) if number != 1]
        widths = [max(3, *(len(row[column]) for row in content)) for column in range(len(rows[0]))]
        for number, row in enumerate(rows):
            cells = [
                "-" * width if number == 1 else cell.ljust(width)
                for cell, width in zip(row, widths, strict=True)
            ]
            aligned.append("| " + " | ".join(cells) + " |")
        index = end
    return "\n".join(aligned)


def _render_template(template: Path, values: Mapping[str, str]) -> dict[str, bytes]:
    """Return every target file named and filled from the template tree."""
    if template.is_symlink() or not template.is_dir():
        fail(f"target template is missing or invalid: {template}")
    files: dict[str, bytes] = {}
    for path in sorted(template.rglob("*")):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            fail(f"target template entry must be a regular file or directory: {path}")
        if path.is_dir():
            continue
        relative = path.relative_to(template).as_posix()
        contents = path.read_bytes()
        if relative.endswith(_TEMPLATE_SUFFIX):
            relative = relative.removesuffix(_TEMPLATE_SUFFIX)
            text = _substitute(contents.decode("utf-8"), values)
            if relative.endswith(".md"):
                text = _align_markdown_tables(text)
            contents = text.encode("utf-8")
        files[_substitute(relative, values)] = contents
    return files


def _select_platform(platform: str | None) -> str:
    """Return the requested platform, or the only one when none was requested."""
    platforms = discover_platforms()
    available = ", ".join(platforms)
    if platform is None:
        if len(platforms) != 1:
            fail(f"choose the platform with --platform: {available}")
        return platforms[0]
    if platform not in platforms:
        fail(f"unknown platform: {platform}; available platforms: {available}")
    return platform


def _skeleton_provider(platform: str) -> ModuleType:
    """Load the module through which one platform supplies its target template."""
    path = common.ROOT / "platforms" / platform / SKELETON_MODULE
    if path.is_symlink() or not path.is_file():
        fail(f"platform {platform} provides no target skeleton: {path}")
    spec = importlib.util.spec_from_file_location("fplinux_platform_target_skeleton", path)
    if spec is None or spec.loader is None:
        fail(f"platform target skeleton cannot be loaded: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if not callable(getattr(module, "template_directory", None)) or not callable(
        getattr(module, "template_values", None)
    ):
        fail(
            "platform target skeleton does not expose template_directory() and "
            f"template_values(target, identity): {path}"
        )
    return module


def create_target(
    name: str,
    *,
    platform: str | None,
    brand: str,
    product: str,
    compatible: str | None,
) -> None:
    """Create a headless skeleton for one phone, then print its files and the next step."""
    if NEW_TARGET_NAME.fullmatch(name) is None:
        fail(
            f"invalid target name: {name}; use lowercase letters and digits "
            "separated by single hyphens"
        )
    provider = _skeleton_provider(_select_platform(platform))
    directory = common.ROOT / "targets" / name
    if directory.exists() or directory.is_symlink():
        fail(f"target {name} already exists")
    try:
        identity = validate_target_identity(
            {
                "brand": brand,
                "product": product,
                "hardware_codes": [],
                "compatible": compatible or derived_compatible(brand, product),
            }
        )
        validate_bootstrap_display_name(identity["display_name"])
    except IdentityError as error:
        fail(str(error))
    files = _render_template(
        provider.template_directory(), provider.template_values(name, identity)
    )

    directory.mkdir()
    try:
        for relative, contents in files.items():
            path = directory / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        load_target(name)
        load_release(name)
    except BaseException:
        shutil.rmtree(directory)
        raise

    print(f"Created headless target targets/{name}:")
    for relative in sorted(files):
        print(f"  {relative}")
    print(f"Next: ./fplinux build {name} && ./fplinux run {name}")
