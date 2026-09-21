# SPDX-License-Identifier: GPL-2.0-only
"""Materialize Linux changes for formatting and destination-aware checks."""

from __future__ import annotations

import argparse
import difflib
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast

from fplinux_cli.build.linux import integration_inputs
from fplinux_cli.build.sources import append_steps, apply_patches, copy_steps
from fplinux_cli.manifests.paths import discover_profiles, discover_targets
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.manifests.values import relative_value

from .common import ROOT, fail
from .workspace import WorkspaceFile

if TYPE_CHECKING:
    from collections.abc import Iterator, Sequence
    from typing import BinaryIO


@dataclass(frozen=True)
class LinuxInput:
    """One existing build integration operation, in its declared order."""

    operation: str
    identity: str
    destination: str
    source: Path

    def destinations(self) -> tuple[str, ...]:
        """Resolve patch paths or the explicit copy/append destination."""
        if self.operation.endswith("patch"):
            return patch_destinations(self.source)
        return (self.destination,)


@dataclass(frozen=True)
class LinuxContext:
    """A target/profile's pinned source and effective integration inputs."""

    target: str
    profile: str
    source: dict[str, Any]
    inputs: tuple[LinuxInput, ...]


def context_inputs(
    target: str, config: dict[str, Any], platform: dict[str, Any]
) -> tuple[LinuxInput, ...]:
    """Use the build's ordering and path resolution for analysis as well."""
    return tuple(LinuxInput(*step) for step in integration_inputs(target, config, platform))


def linux_contexts(selected: frozenset[str]) -> tuple[LinuxContext, ...]:
    """Find every declared Linux context consuming the selected patch paths."""
    with (ROOT / "sources.lock.toml").open("rb") as stream:
        sources = tomllib.load(stream)
    contexts = []
    found: set[str] = set()
    for target in discover_targets():
        for profile in discover_profiles(target):
            config = load_target(target, profile)
            platform = load_platform(config["platform"])
            inputs = context_inputs(target, config, platform)
            matches = selected.intersection(
                step.identity for step in inputs if step.operation.endswith("patch")
            )
            if matches:
                found.update(matches)
                contexts.append(
                    LinuxContext(
                        target, profile, sources[platform["linux"]["source_lock"]], inputs
                    )
                )
    if selected - found:
        fail("not a declared Linux patch: " + ", ".join(sorted(selected - found)))
    return tuple(contexts)


def patch_destinations(path: Path, *, include_deleted: bool = True) -> tuple[str, ...]:
    """Read paths from a text patch, accounting for complete hunk boundaries."""
    result: list[str] = []
    old_remaining = new_remaining = 0
    old_path = ""
    hunk = re.compile(r"^@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@")
    for line in path.read_text().splitlines():
        if old_remaining or new_remaining:
            if line.startswith("\\"):
                continue
            marker = line[:1] or " "
            if marker in {" ", "-"}:
                old_remaining -= 1
            if marker in {" ", "+"}:
                new_remaining -= 1
            if marker not in {" ", "-", "+"} or min(old_remaining, new_remaining) < 0:
                fail(f"malformed Linux patch hunk: {path}")
            continue
        match = hunk.match(line)
        if match:
            old_remaining = int(match.group(1) or "1")
            new_remaining = int(match.group(2) or "1")
        elif line.startswith("--- "):
            old_path = line[4:].split("\t", 1)[0]
        elif line.startswith("+++ ") and old_path:
            new_path = line[4:].split("\t", 1)[0]
            if new_path == "/dev/null" and not include_deleted:
                old_path = ""
                continue
            name = old_path if new_path == "/dev/null" else new_path
            prefix, separator, destination = name.partition("/")
            if not separator or prefix in {"", ".."}:
                fail(f"Linux patch destination has no relative -p1 prefix: {path}")
            result.append(relative_value(destination, "Linux patch destination"))
            old_path = ""
    if old_remaining or new_remaining:
        fail(f"incomplete Linux patch hunk: {path}")
    return tuple(dict.fromkeys(result))


def read_base(archive: Path, version: str, paths: set[str]) -> dict[str, WorkspaceFile]:
    """Read only affected regular files and the pinned kernel style configuration."""
    wanted = paths | {".clang-format"}
    prefix = f"linux-{version}/"
    result = {}
    with tarfile.open(archive, "r|*") as source:
        for member in source:
            name = member.name.removeprefix(prefix)
            if not member.name.startswith(prefix) or name not in wanted:
                continue
            if not member.isfile():
                fail(f"Linux formatter input is not a regular file: {name}")
            stream = cast("BinaryIO", source.extractfile(member))
            with stream:
                result[name] = WorkspaceFile(name, stream.read(), stat.S_IMODE(member.mode))
    if ".clang-format" not in result:
        fail("pinned Linux archive has no .clang-format")
    return result


def write_files(root: Path, files: dict[str, WorkspaceFile]) -> None:
    """Materialize a small source projection, not an additional Linux checkout."""
    for name, source in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(source.contents)
        path.chmod(source.mode)


def file_contents(root: Path, names: Sequence[str]) -> dict[str, WorkspaceFile]:
    """Capture existing files, preserving absence for additions and deletions."""
    return {
        name: WorkspaceFile(
            name, (root / name).read_bytes(), stat.S_IMODE((root / name).stat().st_mode)
        )
        for name in names
        if (root / name).is_file()
    }


def source_diff(
    before: dict[str, WorkspaceFile], after: dict[str, WorkspaceFile], *, context: int = 3
) -> str:
    """Describe source content changes with standard unified-diff generation."""
    chunks = []
    for name in sorted(before.keys() | after.keys()):
        old = (before[name].contents if name in before else b"").decode().splitlines(keepends=True)
        new = (after[name].contents if name in after else b"").decode().splitlines(keepends=True)
        old_name = f"a/{name}" if name in before else "/dev/null"
        new_name = f"b/{name}" if name in after else "/dev/null"
        diff = list(difflib.unified_diff(old, new, old_name, new_name, n=context))
        if diff:
            chunks.append(f"diff --git a/{name} b/{name}\n")
            chunks.extend(
                line if line.endswith("\n") else line + "\n\\ No newline at end of file\n"
                for line in diff
            )
    return "".join(chunks)


def clang_format_diff(root: Path, changes: str, *, write: bool) -> bool:
    """Let LLVM select and format changed C/H lines in their complete source files."""
    binary = shutil.which("clang-format")
    if binary is None:
        fail("clang-format is required for Linux patches")
    tool = Path(binary).resolve().parents[1] / "share/clang/clang-format-diff.py"
    command = [
        sys.executable,
        str(tool),
        "-p1",
        "-regex",
        r".*\.(c|h)",
        "-style",
        f"file:{root / '.clang-format'}",
        "-binary",
        binary,
    ]
    if write:
        command.append("-i")
    result = subprocess.run(
        command, input=changes, text=True, capture_output=True, cwd=root, check=False, timeout=120
    )
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode and not (not write and result.returncode == 1 and result.stdout):
        raise subprocess.CalledProcessError(
            result.returncode, command, result.stdout, result.stderr
        )
    return bool(result.stdout)


def render_patch(
    original: bytes,
    before: dict[str, WorkspaceFile],
    after: dict[str, WorkspaceFile],
    directory: Path,
    *,
    merge_context: int = 0,
) -> bytes:
    """Render a native Git diff with explicit source modes and preserved preamble."""
    before_root, after_root = directory / "a", directory / "b"
    before_root.mkdir()
    after_root.mkdir()
    write_files(before_root, before)
    write_files(after_root, after)
    result = subprocess.run(
        [
            "git",
            "diff",
            "--no-index",
            "--no-prefix",
            "--no-ext-diff",
            "--no-color",
            f"--inter-hunk-context={merge_context}",
            "a",
            "b",
        ],
        cwd=directory,
        capture_output=True,
        check=False,
        timeout=30,
    )
    if result.returncode not in {0, 1}:
        fail(result.stderr.decode())
    header = re.search(rb"^(?:diff --git |--- )", original, re.MULTILINE)
    if header is None:
        fail("Linux patch has no file headers")
    # Empty context lines are accepted by patch without trailing whitespace.
    delta = re.sub(rb"(?m)^ $", b"", result.stdout)
    if not delta:
        fail("formatted patch has no remaining changes; remove its integration entry explicitly")
    return original[: header.start()] + delta


def regenerate_patch(
    original: bytes,
    before: dict[str, WorkspaceFile],
    after: dict[str, WorkspaceFile],
    directory: Path,
) -> bytes:
    """Require the stored diff to reproduce both formatted content and source modes."""
    patch = render_patch(original, before, after, directory)
    candidate = directory / "candidate.patch"
    candidate.write_bytes(patch)
    before_root = directory / "a"
    apply_patches(before_root, [candidate])
    restored = file_contents(before_root, tuple(before.keys() | after.keys()))
    for name in restored.keys() | after.keys():
        if restored.get(name) != after.get(name):
            actual = restored.get(name)
            expected = after.get(name)
            detail = "file presence differs"
            if actual is not None and expected is not None:
                detail = (
                    "contents differ"
                    if actual.contents != expected.contents
                    else f"mode {actual.mode:o}, expected {expected.mode:o}"
                )
            fail(f"formatted patch does not reproduce {name}: {detail}")
    return patch


def project_changes(
    base: dict[str, WorkspaceFile],
    inputs: Sequence[LinuxInput],
    root: Path,
) -> Iterator[tuple[LinuxInput, dict[str, WorkspaceFile], dict[str, WorkspaceFile]]]:
    """Replay the same patch/copy/append sequence used by the public build."""
    write_files(root, base)
    for step in inputs:
        names = step.destinations()
        before = file_contents(root, names)
        if step.operation.endswith("patch"):
            apply_patches(root, [step.source])
        elif step.operation.endswith("copy"):
            copy_steps(root, [(step.source, step.destination)])
        else:
            append_steps(root, [(step.source, step.destination)])
        yield step, before, file_contents(root, names)


def format_context(
    base: dict[str, WorkspaceFile],
    inputs: Sequence[LinuxInput],
    selected: frozenset[str],
) -> dict[str, bytes]:
    """Format selected patch steps and require all following inputs to apply."""
    outputs = {}
    with tempfile.TemporaryDirectory(prefix="fplinux-patch-") as temporary:
        root = Path(temporary) / "linux"
        for index, (step, before, after) in enumerate(project_changes(base, inputs, root)):
            if step.identity not in selected:
                continue
            changes = source_diff(before, after, context=0)
            clang_format_diff(root, changes, write=True)
            # LLVM replaces files through temporaries; formatting must not change their modes.
            for name, source in after.items():
                (root / name).chmod(source.mode)
            formatted = file_contents(root, step.destinations())
            original = step.source.read_bytes()
            if formatted == after:
                outputs[step.identity] = original
            else:
                directory = Path(temporary) / f"diff-{index}"
                directory.mkdir()
                outputs[step.identity] = regenerate_patch(original, before, formatted, directory)
            untouched = [name for name in after if Path(name).suffix not in {".c", ".h"}]
            if untouched:
                print(f"{step.identity}: no autoformatter; retained: {', '.join(untouched)}")
    return outputs


def format_linux_patches(selected: frozenset[str], archives: Path) -> None:
    """Update only selected patches after every consuming context succeeds."""
    contexts = linux_contexts(selected)
    outputs: dict[str, bytes] = {}
    for source_name in {context.source["version"] for context in contexts}:
        matching = [context for context in contexts if context.source["version"] == source_name]
        paths = {
            name for context in matching for step in context.inputs for name in step.destinations()
        }
        base = read_base(archives / f"linux-{source_name}.tar.xz", source_name, paths)
        for context in matching:
            print(f"Linux patch context: {context.target}/{context.profile}")
            for name, contents in format_context(base, context.inputs, selected).items():
                if name in outputs and outputs[name] != contents:
                    fail(f"patch formatting differs between Linux contexts: {name}")
                outputs[name] = contents
    for name, contents in outputs.items():
        (ROOT / name).write_bytes(contents)


def check_linux_changes(
    inputs: Sequence[LinuxInput],
    archive: Path,
    version: str,
    config_diff: Path,
) -> tuple[Path, ...]:
    """Check patch C/H and expose copy/append Kbuild edits in destination context."""
    paths = {name for step in inputs for name in step.destinations()}
    base = read_base(archive, version, paths)
    config_changes = []
    lint_patches = []
    with tempfile.TemporaryDirectory(prefix="fplinux-kernel-style-") as temporary:
        root = Path(temporary) / "linux"
        for index, (step, before, after) in enumerate(project_changes(base, inputs, root)):
            if step.operation.endswith("patch"):
                if clang_format_diff(root, source_diff(before, after, context=0), write=False):
                    fail(f"Linux patch needs formatting: {step.identity}")
                # Keep split macro edits together without linting unrelated file prefixes.
                context = max(
                    (
                        len(item.contents.splitlines())
                        for item in (*before.values(), *after.values())
                    ),
                    default=0,
                )
                staging = Path(temporary) / f"lint-{index}"
                staging.mkdir()
                expanded = render_patch(
                    step.source.read_bytes(), before, after, staging, merge_context=context
                )
                lint_patch = config_diff.parent / "patch-lint" / step.identity
                lint_patch.parent.mkdir(parents=True, exist_ok=True)
                lint_patch.write_bytes(expanded)
                lint_patches.append(lint_patch)
            elif Path(step.destination).name in {"Kconfig", "Makefile", "Kbuild"}:
                config_changes.append(source_diff(before, after))
    config_diff.write_text("".join(config_changes))
    return tuple(lint_patches)


def binding_paths(inputs: Sequence[LinuxInput], source: Path) -> tuple[str, ...]:
    """Select actual binding destinations, regardless of their integration operation."""
    return tuple(
        sorted(
            {
                name
                for step in inputs
                for name in step.destinations()
                if name.startswith("Documentation/devicetree/bindings/")
                and name.endswith(".yaml")
                and (source / name).is_file()
            }
        )
    )


def main() -> None:
    """Run inside the existing private formatter projection."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archives", type=Path, required=True)
    parser.add_argument("patches", nargs="+")
    args = parser.parse_args()
    format_linux_patches(frozenset(args.patches), args.archives)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        fail(error.stderr or str(error))
