# SPDX-License-Identifier: GPL-2.0-only
"""Exercise shared Linux preparation where board patches run through the host GNU patch tool."""

from __future__ import annotations

import subprocess

import pytest
from fplinux_cli.build.kernel import state as linux_state
from fplinux_cli.quality.kernel_patches import read_base

from tests.fixtures.linux_source import LinuxSourceFixture


class SharedLinuxPatchTests(LinuxSourceFixture):
    """Board patches stay causal for every consumer and never publish partial source."""

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
        assert (added) == (source)
        assert ((source / "drivers/replaced.c").read_text()) == ("int gamma = 1;\n")
        (self.root / "targets/gamma/target.toml").unlink()
        restored, _ = self.prepare("alpha")
        assert (restored) == (source)
        assert not ((source / "drivers/gamma.c").exists())
        assert not ((source / "arch/soc/boot/dts/fplinux-gamma-identity.dtsi").exists())
        assert ("GAMMA") not in ((source / "drivers/Kconfig").read_text())
        assert ((source / "drivers/replaced.c").read_text()) == ("upstream original\n")
        assert ((source / "README").stat().st_ino) == (untouched.st_ino)
        assert ((source / "README").stat().st_mtime_ns) == (untouched.st_mtime_ns)
        assert (self.fetch.call_count) == (1)

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
        assert ((source / "drivers/common.c").read_text()) == ("int common = 2;\n")
        assert (after.linux_recipe) != (before.linux_recipe)

    def test_formatter_originals_and_preparation_share_one_extraction(self) -> None:
        """Formatting before a build primes its source; warm original reads write nothing."""
        source_lock = self.sources["linux"]
        original = read_base(self.archive, source_lock, {"drivers/common.c"})
        assert (original["drivers/common.c"].contents) == (b"int common = 1;\n")
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
        assert ((source / "drivers/common.c").read_text()) == ("int common = 2;\n")
        before = self.snapshot(self.cache)
        original = read_base(self.archive, source_lock, {"drivers/common.c", "drivers/alpha.c"})
        assert (original["drivers/common.c"].contents) == (b"int common = 1;\n")
        assert ("drivers/alpha.c") not in (original)
        assert (self.snapshot(self.cache)) == (before)
        assert (self.fetch.call_count) == (0)

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
        with pytest.raises(subprocess.CalledProcessError):
            self.prepare("alpha")
        assert (self.snapshot(source)) == (snapshot)
        linux_state.require_prepared_linux(source, before)
        patch.write_text(
            "--- a/drivers/common.c\n+++ b/drivers/common.c\n@@ -1 +1 @@\n"
            "-int common = 1;\n+int common = 2;\n"
        )
        retried, after = self.prepare("alpha")
        assert (retried) == (source)
        assert ((source / "drivers/common.c").read_text()) == ("int common = 2;\n")
        linux_state.require_prepared_linux(source, after)
        assert (self.fetch.call_count) == (1)
