# SPDX-License-Identifier: GPL-2.0-only
"""Exercise shared Linux preparation with real small archives and projections."""

from __future__ import annotations

import copy
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli import common, linux_state
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import linux as linux_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.kernel_patches import read_base
from fplinux_cli.manifests.linux import discover_linux_targets


class SharedLinuxTests(unittest.TestCase):
    """Source switches reuse one tree while causal image inputs remain independent."""

    def setUp(self) -> None:
        """Create a two-board source fixture; only remote archive fetching is replaced."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / "cache"
        archive_root = self.root / "upstream/linux-test"
        (archive_root / "drivers").mkdir(parents=True)
        for name, contents in {
            "Makefile": "VERSION = test\n",
            ".clang-format": "BasedOnStyle: LLVM\n",
            "README": "untouched upstream\n",
            "drivers/Kconfig": 'menu "Drivers"\nendmenu\n',
            "drivers/common.c": "int common = 1;\n",
            "drivers/replaced.c": "upstream original\n",
        }.items():
            (archive_root / name).write_text(contents)
        self.archive = self.root / "linux-test.tar.xz"
        with tarfile.open(self.archive, "w:xz") as output:
            output.add(archive_root, arcname="linux-test")
        self.sources: dict[str, Any] = {
            "linux": {
                "version": "test",
                "sha256": common.sha256_file(self.archive),
                "url": "https://example.invalid/linux-test.tar.xz",
            }
        }
        self.platform("soc")
        self.target("alpha")
        self.target("beta")
        self.enterContext(mock.patch.object(common, "ROOT", self.root))
        self.enterContext(mock.patch.object(inputs_build, "CACHE", self.cache))
        self.fetch = self.enterContext(
            mock.patch.object(sources_build, "fetch", return_value=self.archive)
        )

    def platform(self, name: str, *, source_lock: str = "linux") -> None:
        """Write the Linux-only platform manifest consumed by real discovery."""
        directory = self.root / "platforms" / name
        directory.mkdir(parents=True)
        (directory / "platform.toml").write_text(
            f"""[identity]
vendor = "Example"
soc = "{name.upper()}"
aliases = []
compatible = "example,{name}"
[linux]
arch = "arm"
source_lock = "{source_lock}"
dts_directory = "arch/{name}/boot/dts"
platform_identity_header = "include/{name}-identity.h"
patches = []
copies = []
appends = []
"""
        )

    def target(self, name: str, *, platform: str = "soc", extra: str = "") -> None:
        """Add a board with one independent driver and one guarded Kconfig fragment."""
        directory = self.root / "targets" / name
        directory.mkdir(parents=True)
        (directory / "driver.c").write_text(f"int {name} = 1;\n")
        (directory / "fragment").write_text(f'config {name.upper()}\n\tbool "{name}"\n')
        (directory / "target.toml").write_text(
            f"""platform = "{platform}"
[identity]
brand = "Example"
product = "{name}"
hardware_codes = []
compatible = "example,{name}"
[linux]
patches = []
[[linux.copies]]
source = "driver.c"
destination = "drivers/{name}.c"
[[linux.appends]]
source = "fragment"
destination = "drivers/Kconfig"
{extra}"""
        )

    def prepare(
        self, name: str, *, external: bool = False
    ) -> tuple[Path, linux_state.PreparedLinuxState]:
        """Load a selected Linux context and run the production preparer."""
        digest = self.sources["linux"]["sha256"]
        selected = next(
            target
            for target in discover_linux_targets(self.root, self.sources, digest)
            if target.name == name
        )
        config = copy.deepcopy(selected.config)
        config["linux"]["root"] = (
            {
                "kind": "external",
                "partuuid": "12345678-02",
                "filesystem": "ext4",
                "wait_seconds": 5,
            }
            if external
            else {"kind": "initramfs"}
        )
        config["profile"] = "sd" if external else "default"
        return linux_build.prepare_linux(self.sources, name, config, selected.platform)

    @staticmethod
    def snapshot(source: Path) -> dict[str, tuple[bytes, int, int]]:
        """Observe all files in this small fixture, including source inode and mtime."""
        return {
            path.relative_to(source).as_posix(): (
                path.read_bytes(),
                path.stat().st_ino,
                path.stat().st_mtime_ns,
            )
            for path in source.rglob("*")
            if path.is_file()
        }

    def test_board_and_profile_switches_leave_shared_source_unchanged(self) -> None:
        """A/B/A and profile switches need neither another archive nor source writes."""
        source, first = self.prepare("alpha")
        before = self.snapshot(source)
        self.archive.unlink()
        for name, external in (("beta", False), ("alpha", True), ("alpha", False)):
            with self.subTest(name=name, external=external):
                current, state = self.prepare(name, external=external)
                self.assertEqual(current, source)
                self.assertEqual(self.snapshot(source), before)
                linux_state.require_prepared_linux(source, state)
                if name == "alpha" and external:
                    self.assertNotEqual(state.linux_recipe, first.linux_recipe)
        self.assertEqual(self.fetch.call_count, 1)
        self.assertEqual(len(tuple((self.cache / "linux/sources").iterdir())), 1)
        self.assertEqual((source / "drivers/alpha.c").read_text(), "int alpha = 1;\n")
        self.assertEqual((source / "drivers/beta.c").read_text(), "int beta = 1;\n")
        self.assertEqual(
            (source / "drivers/Kconfig").read_text(),
            'menu "Drivers"\nendmenu\n\nconfig ALPHA\n\tbool "alpha"\n'
            '\nconfig BETA\n\tbool "beta"\n',
        )
        self.assertIn(
            b"Example alpha",
            (source / "arch/soc/boot/dts/fplinux-alpha-identity.dtsi").read_bytes(),
        )
        self.assertIn(
            b"Example beta", (source / "arch/soc/boot/dts/fplinux-beta-identity.dtsi").read_bytes()
        )
        self.assertFalse((source / "arch/soc/boot/dts/fplinux-root.dtsi").exists())

    def test_peer_driver_edit_changes_only_its_destination_and_own_recipe(self) -> None:
        """An independent board edit preserves other boards' build identities and source mtimes."""
        source, alpha_before = self.prepare("alpha")
        _, beta_before = self.prepare("beta")
        before = self.snapshot(source)
        (self.root / "targets/beta/driver.c").write_text("int beta = 2;\n")
        _, alpha_after = self.prepare("alpha")
        _, beta_after = self.prepare("beta")
        after = self.snapshot(source)
        changes = {
            name
            for name in before
            if before[name] != after[name] and not name.startswith(".fplinux-")
        }
        self.assertEqual(changes, {"drivers/beta.c"})
        self.assertEqual(alpha_after.linux_recipe, alpha_before.linux_recipe)
        self.assertNotEqual(beta_after.linux_recipe, beta_before.linux_recipe)
        self.assertNotEqual(alpha_after.tree_recipe, alpha_before.tree_recipe)
        self.assertEqual(self.fetch.call_count, 1)
        with self.assertRaisesRegex(linux_state.LinuxStateError, "changed after preparation"):
            linux_state.require_prepared_linux(source, alpha_before)

    def test_added_and_removed_board_updates_fragments_and_restores_originals(self) -> None:
        """New boards add small files; removing their integration restores upstream bytes."""
        source, _ = self.prepare("alpha")
        untouched = (source / "README").stat()
        self.target("gamma")
        directory = self.root / "targets/gamma"
        manifest = directory / "target.toml"
        manifest.write_text(
            manifest.read_text().replace("patches = []", 'patches = ["replace.patch"]')
        )
        (directory / "replace.patch").write_text(
            "--- a/drivers/replaced.c\n+++ b/drivers/replaced.c\n@@ -1 +1 @@\n"
            "-upstream original\n+int gamma = 1;\n"
        )
        added, _ = self.prepare("gamma")
        self.assertEqual(added, source)
        self.assertEqual((source / "drivers/replaced.c").read_text(), "int gamma = 1;\n")
        (self.root / "targets/gamma/target.toml").unlink()
        restored, _ = self.prepare("alpha")
        self.assertEqual(restored, source)
        self.assertFalse((source / "drivers/gamma.c").exists())
        self.assertFalse((source / "arch/soc/boot/dts/fplinux-gamma-identity.dtsi").exists())
        self.assertNotIn("GAMMA", (source / "drivers/Kconfig").read_text())
        self.assertEqual((source / "drivers/replaced.c").read_text(), "upstream original\n")
        self.assertEqual((source / "README").stat().st_ino, untouched.st_ino)
        self.assertEqual((source / "README").stat().st_mtime_ns, untouched.st_mtime_ns)
        self.assertEqual(self.fetch.call_count, 1)

    def test_two_platforms_share_source_and_another_base_uses_its_own_slot(self) -> None:
        """Source identity, not platform identity, chooses the complete Linux tree."""
        self.platform("other")
        self.target("delta", platform="other")
        source, _ = self.prepare("alpha")
        other, _ = self.prepare("delta")
        self.assertEqual(other, source)
        self.assertTrue((source / "arch/other/boot/dts/fplinux-delta-identity.dtsi").is_file())
        with self.archive.open("ab") as archive:
            archive.write(b"\0")
        self.sources["linux"]["sha256"] = common.sha256_file(self.archive)
        changed, _ = self.prepare("alpha")
        self.assertNotEqual(changed, source)
        self.assertTrue((source / "drivers/alpha.c").is_file())
        self.assertEqual(self.fetch.call_count, 2)

    def test_peer_patch_to_upstream_code_changes_selected_recipe(self) -> None:
        """Shared upstream code stays causal even when its patch is owned by another board."""
        source, before = self.prepare("alpha")
        target = self.root / "targets/beta"
        (target / "target.toml").write_text(
            (target / "target.toml")
            .read_text()
            .replace("patches = []", 'patches = ["common.patch"]')
        )
        (target / "common.patch").write_text(
            "--- a/drivers/common.c\n+++ b/drivers/common.c\n@@ -1 +1 @@\n"
            "-int common = 1;\n+int common = 2;\n"
        )
        _, after = self.prepare("alpha")
        self.assertEqual((source / "drivers/common.c").read_text(), "int common = 2;\n")
        self.assertNotEqual(after.linux_recipe, before.linux_recipe)

    def test_other_platform_append_to_common_code_changes_selected_recipe(self) -> None:
        """Platform appends to shared upstream code remain causal for every consumer."""
        self.platform("other")
        self.target("delta", platform="other")
        directory = self.root / "platforms/other"
        manifest = directory / "platform.toml"
        manifest.write_text(
            manifest.read_text().replace("appends = []\n", "")
            + '\n[[linux.appends]]\nsource = "platforms/other/common.fragment"\n'
            'destination = "drivers/common.c"\n'
        )
        fragment = directory / "common.fragment"
        fragment.write_text("int other = 1;\n")
        source, before = self.prepare("alpha")
        fragment.write_text("int other = 2;\n")
        _, after = self.prepare("alpha")
        self.assertEqual(
            (source / "drivers/common.c").read_text(), "int common = 1;\n\nint other = 2;\n"
        )
        self.assertNotEqual(after.linux_recipe, before.linux_recipe)

    def test_peer_board_fragment_change_invalidates_shared_config_recipe(self) -> None:
        """The shared Kconfig input cannot hide a peer fragment's changed defaults."""
        source, before = self.prepare("alpha")
        (self.root / "targets/beta/fragment").write_text(
            'config BETA\n\tbool "beta"\n\tdefault y\n'
        )
        _, after = self.prepare("alpha")
        self.assertIn("\tdefault y\n", (source / "drivers/Kconfig").read_text())
        self.assertNotEqual(after.linux_recipe, before.linux_recipe)

    def test_formatter_originals_and_preparation_share_one_extraction(self) -> None:
        """Formatting before a build primes its source; warm original reads write nothing."""
        source_lock = self.sources["linux"]
        original = read_base(self.archive, source_lock, {"drivers/common.c"})
        self.assertEqual(original["drivers/common.c"].contents, b"int common = 1;\n")
        self.archive.unlink()
        target = self.root / "targets/beta"
        manifest = target / "target.toml"
        manifest.write_text(
            manifest.read_text().replace("patches = []", 'patches = ["common.patch"]')
        )
        (target / "common.patch").write_text(
            "--- a/drivers/common.c\n+++ b/drivers/common.c\n@@ -1 +1 @@\n"
            "-int common = 1;\n+int common = 2;\n"
        )
        source, _ = self.prepare("alpha")
        self.assertEqual((source / "drivers/common.c").read_text(), "int common = 2;\n")
        before = self.snapshot(self.cache)
        original = read_base(self.archive, source_lock, {"drivers/common.c", "drivers/alpha.c"})
        self.assertEqual(original["drivers/common.c"].contents, b"int common = 1;\n")
        self.assertNotIn("drivers/alpha.c", original)
        self.assertEqual(self.snapshot(self.cache), before)
        self.assertEqual(self.fetch.call_count, 0)

    def test_failed_patch_keeps_completed_source_and_can_be_retried(self) -> None:
        """A normal patch error never publishes partial source or a successful new receipt."""
        source, before = self.prepare("alpha")
        snapshot = self.snapshot(source)
        target = self.root / "targets/beta"
        original = (target / "target.toml").read_text()
        (target / "target.toml").write_text(
            original.replace("patches = []", 'patches = ["bad.patch"]')
        )
        patch = target / "bad.patch"
        patch.write_text(
            "--- a/drivers/common.c\n+++ b/drivers/common.c\n@@ -1 +1 @@\n"
            "-int nonexistent = 1;\n+int common = 2;\n"
        )
        with self.assertRaises(subprocess.CalledProcessError):
            self.prepare("alpha")
        self.assertEqual(self.snapshot(source), snapshot)
        linux_state.require_prepared_linux(source, before)
        patch.write_text(
            "--- a/drivers/common.c\n+++ b/drivers/common.c\n@@ -1 +1 @@\n"
            "-int common = 1;\n+int common = 2;\n"
        )
        retried, after = self.prepare("alpha")
        self.assertEqual(retried, source)
        self.assertEqual((source / "drivers/common.c").read_text(), "int common = 2;\n")
        linux_state.require_prepared_linux(source, after)
        self.assertEqual(self.fetch.call_count, 1)

    def test_conflicting_copy_owners_fail_before_source_publication(self) -> None:
        """Discovery order cannot silently choose between different board source owners."""
        path = self.root / "targets/beta/target.toml"
        path.write_text(path.read_text().replace("drivers/beta.c", "drivers/alpha.c"))
        with self.assertRaisesRegex(SystemExit, "multiple owners: drivers/alpha.c"):
            self.prepare("alpha")
        self.assertFalse((self.cache / "linux/sources").exists())

    def test_board_copy_cannot_replace_an_upstream_file(self) -> None:
        """An independent board copy cannot bypass shared-code causal tracking."""
        source, prepared = self.prepare("alpha")
        before = self.snapshot(source)
        manifest = self.root / "targets/beta/target.toml"
        manifest.write_text(manifest.read_text().replace("drivers/beta.c", "drivers/common.c"))
        with self.assertRaisesRegex(SystemExit, "target Linux copy must add a new board file"):
            self.prepare("alpha")
        self.assertEqual(self.snapshot(source), before)
        self.assertEqual((source / "drivers/common.c").read_text(), "int common = 1;\n")
        linux_state.require_prepared_linux(source, prepared)

    def test_peer_platform_new_header_contents_and_destination_are_causal(self) -> None:
        """Shared copied headers invalidate consumers even when upstream had no such path."""
        self.platform("other")
        self.target("delta", platform="other")
        directory = self.root / "platforms/other"
        manifest = directory / "platform.toml"
        manifest.write_text(
            manifest.read_text().replace("copies = []\n", "")
            + '\n[[linux.copies]]\nsource = "platforms/other/shared.h"\n'
            'destination = "include/shared.h"\n'
        )
        header = directory / "shared.h"
        header.write_text("#define SHARED_VALUE 1\n")
        (self.root / "targets/alpha/driver.c").write_text(
            "#include <shared.h>\nint alpha = SHARED_VALUE;\n"
        )
        source, before = self.prepare("alpha")
        header.write_text("#define SHARED_VALUE 2\n")
        _, changed = self.prepare("alpha")
        self.assertEqual((source / "include/shared.h").read_text(), "#define SHARED_VALUE 2\n")
        self.assertNotEqual(changed.linux_recipe, before.linux_recipe)
        manifest.write_text(
            manifest.read_text().replace("include/shared.h", "include/moved-shared.h")
        )
        _, moved = self.prepare("alpha")
        self.assertFalse((source / "include/shared.h").exists())
        self.assertEqual(
            (source / "include/moved-shared.h").read_text(), "#define SHARED_VALUE 2\n"
        )
        self.assertNotEqual(moved.linux_recipe, changed.linux_recipe)

    def test_profile_root_is_written_only_to_build_output_and_keeps_mtime_on_reuse(self) -> None:
        """Separate build directories retain their own root arguments without source mutation."""
        source, _ = self.prepare("alpha")
        before = self.snapshot(source)
        config = {"linux": {"root": {"kind": "initramfs"}}}
        output = self.root / "out/default"
        path = linux_state.write_profile_root(output, config)
        first = path.stat()
        self.assertIn(b"rdinit=/init", path.read_bytes())
        self.assertEqual(
            linux_state.write_profile_root(output, config).stat().st_mtime_ns, first.st_mtime_ns
        )
        external = {
            "linux": {
                "root": {
                    "kind": "external",
                    "partuuid": "12345678-02",
                    "filesystem": "ext4",
                    "wait_seconds": 5,
                }
            }
        }
        second = linux_state.write_profile_root(self.root / "out/sd", external)
        self.assertIn(
            b"root=PARTUUID=12345678-02 rootfstype=ext4 rootwait=5 rw", second.read_bytes()
        )
        self.assertIn(b"rdinit=/init", path.read_bytes())
        self.assertEqual(self.snapshot(source), before)


if __name__ == "__main__":
    unittest.main()
