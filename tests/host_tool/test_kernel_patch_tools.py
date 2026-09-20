# SPDX-License-Identifier: GPL-2.0-only
"""Exercise patch formatting with real LLVM, Git and GNU patch tools."""

from __future__ import annotations

import io
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from fplinux_cli.common import ROOT
from fplinux_cli.kernel_patches import (
    LinuxInput,
    binding_paths,
    check_linux_changes,
    file_contents,
    format_context,
    project_changes,
    source_diff,
)
from fplinux_cli.workspace import WorkspaceFile


class KernelPatchToolTests(unittest.TestCase):
    """Check resulting files, not particular hunk spellings or internal tool calls."""

    def setUp(self) -> None:
        """Create isolated source inputs with the repository's pinned C style."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.base = {
            ".clang-format": WorkspaceFile(
                ".clang-format", (ROOT / ".clang-format").read_bytes(), 0o644
            ),
            "drivers/example.c": WorkspaceFile(
                "drivers/example.c",
                b"int untouched(void){return 41;}\n\n\n\n\n\n"
                b"int answer(void)\n{\n\treturn 1;\n}\n",
                0o640,
            ),
            "drivers/Kconfig": WorkspaceFile(
                "drivers/Kconfig", b'config ORIGINAL\n\tbool "Original"\n', 0o644
            ),
        }

    def patch(
        self, name: str, before: dict[str, WorkspaceFile], after: dict[str, WorkspaceFile]
    ) -> LinuxInput:
        """Write a real text patch consumed by GNU patch in the test."""
        path = self.root / name
        path.write_text(source_diff(before, after))
        return LinuxInput("platform-patch", name, "", path)

    def changed_source(self, body: bytes) -> dict[str, WorkspaceFile]:
        """Change one return statement without touching the distant function."""
        result = dict(self.base)
        old = result["drivers/example.c"]
        result[old.path] = WorkspaceFile(
            old.path, old.contents.replace(b"\treturn 1;", body), old.mode
        )
        return result

    def test_mixed_patch_formats_c_preserves_neighbors_modes_and_is_idempotent(self) -> None:
        """Only affected C statements change; Kconfig, preamble and source modes survive."""
        after = self.changed_source(b" return  7;")
        config = b'config ORIGINAL\n\tbool "Original"\n\nconfig NEW\n    bool "New"\n'
        after["drivers/Kconfig"] = WorkspaceFile("drivers/Kconfig", config, 0o644)
        step = self.patch("mixed.patch", self.base, after)
        preamble = b"Subject: [PATCH] Add a sample operation\n\nDescribe the sample operation.\n\n"
        step.source.write_bytes(preamble + step.source.read_bytes())
        original = step.source.read_bytes()

        formatted = format_context(self.base, [step], frozenset({step.identity}))[step.identity]
        self.assertNotEqual(formatted, original)
        self.assertEqual(step.source.read_bytes(), original)
        self.assertTrue(formatted.startswith(preamble))
        step.source.write_bytes(formatted)
        destination = self.root / "result"
        list(project_changes(self.base, [step], destination))
        self.assertEqual(
            (destination / "drivers/example.c").read_bytes(),
            b"int untouched(void){return 41;}\n\n\n\n\n\nint answer(void)\n{\n\treturn 7;\n}\n",
        )
        self.assertEqual((destination / "drivers/example.c").stat().st_mode & 0o777, 0o640)
        self.assertEqual((destination / "drivers/Kconfig").read_bytes(), config)
        self.assertEqual(
            format_context(self.base, [step], frozenset({step.identity}))[step.identity], formatted
        )

    def test_following_patch_conflict_does_not_rewrite_selected_inputs(self) -> None:
        """Formatting must not silently rebase a subsequent dependent patch."""
        middle = self.changed_source(b" return  7;")
        final = self.changed_source(b" return  8;")
        first = self.patch("first.patch", self.base, middle)
        second = self.patch("second.patch", middle, final)
        original = {step.source: step.source.read_bytes() for step in (first, second)}
        with self.assertRaises(subprocess.CalledProcessError):
            format_context(self.base, [first, second], frozenset({first.identity}))
        self.assertEqual({path: path.read_bytes() for path in original}, original)

    def test_adjacent_unchanged_function_is_not_pulled_into_formatting(self) -> None:
        """A context line in a patch must not become an explicit formatter range."""
        name = "drivers/example.c"
        self.base[name] = WorkspaceFile(
            name, b"int unrelated(void){return 4;}\nint value = 1;\n", 0o644
        )
        after = dict(self.base)
        after[name] = WorkspaceFile(name, b"int unrelated(void){return 4;}\nint value=2;\n", 0o644)
        step = self.patch("adjacent.patch", self.base, after)
        formatted = format_context(self.base, [step], frozenset({step.identity}))[step.identity]
        step.source.write_bytes(formatted)
        destination = self.root / "result"
        list(project_changes(self.base, [step], destination))
        self.assertEqual(
            (destination / name).read_bytes(), b"int unrelated(void){return 4;}\nint value = 2;\n"
        )

    def test_deleted_binding_is_not_selected_for_final_schema_validation(self) -> None:
        """Removing an obsolete binding does not ask a validator to reopen the deleted file."""
        name = "Documentation/devicetree/bindings/misc/fixture.yaml"
        before = {name: WorkspaceFile(name, b"description: obsolete\n", 0o644)}
        step = self.patch("delete.patch", before, {})
        destination = self.root / "result"
        list(project_changes(before, [step], destination))
        self.assertFalse((destination / name).exists())
        self.assertEqual(binding_paths([step], destination), ())

    def archive(self) -> Path:
        """Package the fixture in the same source layout as the pinned kernel."""
        path = self.root / "linux.tar.xz"
        with tarfile.open(path, "w:xz") as archive:
            for name, source in self.base.items():
                member = tarfile.TarInfo(f"linux-fixture/{name}")
                member.size = len(source.contents)
                member.mode = source.mode
                archive.addfile(member, io.BytesIO(source.contents))
        return path

    def test_check_rejects_unformatted_patch_without_writing_it(self) -> None:
        """The check path catches the same C formatting defect as the formatter."""
        step = self.patch("bad.patch", self.base, self.changed_source(b" return  7;"))
        original = step.source.read_bytes()
        with self.assertRaisesRegex(SystemExit, "Linux patch needs formatting: bad.patch"):
            check_linux_changes([step], self.archive(), "fixture", self.root / "config.patch")
        self.assertEqual(step.source.read_bytes(), original)

    def test_append_is_checked_as_a_change_to_destination_kconfig(self) -> None:
        """An arbitrary fragment filename cannot bypass destination-aware Kconfig checks."""
        fragment = self.root / "feature.fragment"
        fragment.write_bytes(b'config APPENDED\n\tbool "Appended"\n')
        step = LinuxInput("platform-append", "feature.fragment", "drivers/Kconfig", fragment)
        delta = self.root / "config.patch"
        check_linux_changes([step], self.archive(), "fixture", delta)
        generated = LinuxInput("platform-patch", "config.patch", "", delta)
        destination = self.root / "result"
        list(project_changes(self.base, [generated], destination))
        self.assertEqual(
            file_contents(destination, ("drivers/Kconfig",))["drivers/Kconfig"].contents,
            b'config ORIGINAL\n\tbool "Original"\n\nconfig APPENDED\n\tbool "Appended"\n',
        )


if __name__ == "__main__":
    unittest.main()
