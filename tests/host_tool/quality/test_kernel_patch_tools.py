# SPDX-License-Identifier: GPL-2.0-only
"""Exercise patch formatting with real LLVM, Git and GNU patch tools."""

from __future__ import annotations

import io
import subprocess
import tarfile
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build.kernel.projection import LinuxInput, file_contents, project_changes
from fplinux_cli.common import ROOT, sha256_file
from fplinux_cli.quality.kernel_patches import (
    binding_paths,
    check_linux_changes,
    format_context,
    source_diff,
)
from fplinux_cli.workspace.capture import WorkspaceFile

if TYPE_CHECKING:
    from collections.abc import Iterator


FIXTURES = ROOT / "tests/fixtures/quality/host_tool/kernel-patch"


@pytest.fixture
def kernel_root(monkeypatch: pytest.MonkeyPatch) -> Iterator[Path]:
    """Keep projected sources and the build cache isolated for one case."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        monkeypatch.setattr(inputs_build, "CACHE", root / "cache")
        yield root


@pytest.fixture
def kernel_base() -> dict[str, WorkspaceFile]:
    """Prepare independent source bytes with the repository's pinned C style."""
    return {
        "Makefile": WorkspaceFile("Makefile", b"VERSION = fixture\n", 0o644),
        ".clang-format": WorkspaceFile(
            ".clang-format", (ROOT / ".clang-format").read_bytes(), 0o644
        ),
        "drivers/example.c": WorkspaceFile(
            "drivers/example.c", (FIXTURES / "base.c.fixture").read_bytes(), 0o640
        ),
        "drivers/Kconfig": WorkspaceFile(
            "drivers/Kconfig", (FIXTURES / "base.Kconfig.fixture").read_bytes(), 0o644
        ),
    }


class KernelPatchToolTests:
    """Check resulting files, not particular hunk spellings or internal tool calls."""

    def patch(
        self,
        root: Path,
        name: str,
        before: dict[str, WorkspaceFile],
        after: dict[str, WorkspaceFile],
    ) -> LinuxInput:
        """Write a real text patch consumed by GNU patch in the test."""
        path = root / name
        path.write_text(source_diff(before, after))
        return LinuxInput("platform-patch", name, "", path)

    def changed_source(
        self, base: dict[str, WorkspaceFile], body: bytes
    ) -> dict[str, WorkspaceFile]:
        """Change one return statement without touching the distant function."""
        result = dict(base)
        old = result["drivers/example.c"]
        result[old.path] = WorkspaceFile(
            old.path, old.contents.replace(b"\treturn 1;", body), old.mode
        )
        return result

    def test_mixed_patch_formats_c_preserves_neighbors_modes_and_is_idempotent(
        self, kernel_root: Path, kernel_base: dict[str, WorkspaceFile]
    ) -> None:
        """Only affected C statements change; Kconfig, preamble and source modes survive."""
        after = self.changed_source(kernel_base, b" return  7;")
        config = (FIXTURES / "changed.Kconfig.fixture").read_bytes()
        after["drivers/Kconfig"] = WorkspaceFile("drivers/Kconfig", config, 0o644)
        step = self.patch(kernel_root, "mixed.patch", kernel_base, after)
        preamble = b"Subject: [PATCH] Add a sample operation\n\nDescribe the sample operation.\n\n"
        step.source.write_bytes(preamble + step.source.read_bytes())
        original = step.source.read_bytes()

        formatted = format_context(kernel_base, [step], frozenset({step.identity}))[step.identity]
        assert (formatted) != (original)
        assert (step.source.read_bytes()) == (original)
        assert formatted.startswith(preamble)
        step.source.write_bytes(formatted)
        destination = kernel_root / "result"
        list(project_changes(kernel_base, [step], destination))
        assert ((destination / "drivers/example.c").read_bytes()) == (
            (FIXTURES / "formatted.c.fixture").read_bytes()
        )
        assert ((destination / "drivers/example.c").stat().st_mode & 0o777) == (0o640)
        assert ((destination / "drivers/Kconfig").read_bytes()) == (config)
        assert (
            format_context(kernel_base, [step], frozenset({step.identity}))[step.identity]
        ) == (formatted)

    def test_following_patch_conflict_does_not_rewrite_selected_inputs(
        self, kernel_root: Path, kernel_base: dict[str, WorkspaceFile]
    ) -> None:
        """Formatting must not silently rebase a subsequent dependent patch."""
        middle = self.changed_source(kernel_base, b" return  7;")
        final = self.changed_source(kernel_base, b" return  8;")
        first = self.patch(kernel_root, "first.patch", kernel_base, middle)
        second = self.patch(kernel_root, "second.patch", middle, final)
        original = {step.source: step.source.read_bytes() for step in (first, second)}
        with pytest.raises(subprocess.CalledProcessError):
            format_context(kernel_base, [first, second], frozenset({first.identity}))
        assert ({path: path.read_bytes() for path in original}) == (original)

    def test_adjacent_unchanged_function_is_not_pulled_into_formatting(
        self, kernel_root: Path, kernel_base: dict[str, WorkspaceFile]
    ) -> None:
        """A context line in a patch must not become an explicit formatter range."""
        name = "drivers/example.c"
        kernel_base[name] = WorkspaceFile(
            name, (FIXTURES / "adjacent-input.c.fixture").read_bytes(), 0o644
        )
        after = dict(kernel_base)
        after[name] = WorkspaceFile(
            name, (FIXTURES / "adjacent-changed.c.fixture").read_bytes(), 0o644
        )
        step = self.patch(kernel_root, "adjacent.patch", kernel_base, after)
        formatted = format_context(kernel_base, [step], frozenset({step.identity}))[step.identity]
        step.source.write_bytes(formatted)
        destination = kernel_root / "result"
        list(project_changes(kernel_base, [step], destination))
        assert ((destination / name).read_bytes()) == (
            (FIXTURES / "adjacent-expected.c.fixture").read_bytes()
        )

    def test_deleted_binding_is_not_selected_for_final_schema_validation(
        self, kernel_root: Path
    ) -> None:
        """Removing an obsolete binding does not ask a validator to reopen the deleted file."""
        name = "Documentation/devicetree/bindings/misc/fixture.yaml"
        before = {name: WorkspaceFile(name, b"description: obsolete\n", 0o644)}
        step = self.patch(kernel_root, "delete.patch", before, {})
        destination = kernel_root / "result"
        list(project_changes(before, [step], destination))
        assert not ((destination / name).exists())
        assert (binding_paths([step], destination)) == (())

    def archive(self, root: Path, base: dict[str, WorkspaceFile]) -> Path:
        """Package the fixture in the same source layout as the pinned kernel."""
        path = root / "linux.tar.xz"
        with tarfile.open(path, "w:xz") as archive:
            for name, source in base.items():
                member = tarfile.TarInfo(f"linux-fixture/{name}")
                member.size = len(source.contents)
                member.mode = source.mode
                archive.addfile(member, io.BytesIO(source.contents))
        return path

    def test_check_rejects_unformatted_patch_without_writing_it(
        self, kernel_root: Path, kernel_base: dict[str, WorkspaceFile]
    ) -> None:
        """The check path catches the same C formatting defect as the formatter."""
        changed = self.changed_source(kernel_base, b" return  7;")
        step = self.patch(kernel_root, "bad.patch", kernel_base, changed)
        original = step.source.read_bytes()
        archive = self.archive(kernel_root, kernel_base)
        source = {"version": "fixture", "sha256": sha256_file(archive)}
        with pytest.raises(SystemExit, match=r"Linux patch needs formatting: bad.patch"):
            check_linux_changes([step], archive, source, kernel_root / "config.patch")
        assert (step.source.read_bytes()) == (original)

    def test_appended_fragment_generates_destination_kconfig_diff(
        self, kernel_root: Path, kernel_base: dict[str, WorkspaceFile]
    ) -> None:
        """A fragment targeting Kconfig is projected into that destination's diff."""
        fragment = kernel_root / "feature.fragment"
        fragment.write_bytes((FIXTURES / "append.Kconfig.fixture").read_bytes())
        step = LinuxInput("platform-append", "feature.fragment", "drivers/Kconfig", fragment)
        delta = kernel_root / "config.patch"
        archive = self.archive(kernel_root, kernel_base)
        source = {"version": "fixture", "sha256": sha256_file(archive)}
        check_linux_changes([step], archive, source, delta)
        generated = LinuxInput("platform-patch", "config.patch", "", delta)
        destination = kernel_root / "result"
        list(project_changes(kernel_base, [generated], destination))
        assert (file_contents(destination, ("drivers/Kconfig",))["drivers/Kconfig"].contents) == (
            (FIXTURES / "appended.Kconfig.fixture").read_bytes()
        )
