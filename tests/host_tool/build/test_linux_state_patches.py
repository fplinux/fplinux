# SPDX-License-Identifier: GPL-2.0-only
"""Exercise shared Linux preparation where board patches run through the host GNU patch tool."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.kernel import state as linux_state
from fplinux_cli.quality.kernel_patches import read_base

from tests import ROOT
from tests.fixtures.linux_source import (
    linux_source as linux_source,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.fixtures.linux_source import LinuxSourceContext

PATCHES = ROOT / "tests/host_tool/build/fixtures/linux-state-patches"


class SharedLinuxPatchTests:
    """Board patches stay causal for every consumer and never publish partial source."""

    def test_added_and_removed_board_updates_fragments_and_restores_originals(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """New boards add small files; removing their integration restores upstream bytes."""
        source, _ = linux_source.prepare("alpha")
        untouched = (source / "README").stat()
        linux_source.target("gamma")
        directory = linux_source.root / "targets/gamma"
        manifest = directory / "target.toml"
        manifest.write_text(
            manifest.read_text().replace("patches = []", 'patches = ["replace.patch"]')
        )
        (directory / "replace.patch").write_bytes((PATCHES / "replace.patch").read_bytes())
        added, _ = linux_source.prepare("gamma")
        assert (added) == (source)
        assert ((source / "drivers/replaced.c").read_text()) == ("int gamma = 1;\n")
        (linux_source.root / "targets/gamma/target.toml").unlink()
        restored, _ = linux_source.prepare("alpha")
        assert (restored) == (source)
        assert not ((source / "drivers/gamma.c").exists())
        assert not ((source / "arch/soc/boot/dts/fplinux-gamma-identity.dtsi").exists())
        assert ("GAMMA") not in ((source / "drivers/Kconfig").read_text())
        assert ((source / "drivers/replaced.c").read_text()) == ("upstream original\n")
        assert ((source / "README").stat().st_ino) == (untouched.st_ino)
        assert ((source / "README").stat().st_mtime_ns) == (untouched.st_mtime_ns)
        assert (linux_source.fetch.call_count) == (1)

    def test_peer_patch_to_upstream_code_changes_selected_recipe(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """Shared upstream code stays causal even when its patch is owned by another board."""
        source, before = linux_source.prepare("alpha")
        target = linux_source.root / "targets/beta"
        (target / "target.toml").write_text(
            (target / "target.toml")
            .read_text()
            .replace("patches = []", 'patches = ["common.patch"]')
        )
        (target / "common.patch").write_bytes((PATCHES / "common.patch").read_bytes())
        _, after = linux_source.prepare("alpha")
        assert ((source / "drivers/common.c").read_text()) == ("int common = 2;\n")
        assert (after.linux_recipe) != (before.linux_recipe)

    def test_formatter_originals_and_preparation_share_one_extraction(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """Formatting before a build primes its source; warm original reads write nothing."""
        source_lock = linux_source.sources["linux"]
        original = read_base(linux_source.archive, source_lock, {"drivers/common.c"})
        assert (original["drivers/common.c"].contents) == (b"int common = 1;\n")
        linux_source.archive.unlink()
        target = linux_source.root / "targets/beta"
        manifest = target / "target.toml"
        manifest.write_text(
            manifest.read_text().replace("patches = []", 'patches = ["common.patch"]')
        )
        (target / "common.patch").write_bytes((PATCHES / "common.patch").read_bytes())
        source, _ = linux_source.prepare("alpha")
        assert ((source / "drivers/common.c").read_text()) == ("int common = 2;\n")
        before = linux_source.snapshot(linux_source.cache)
        original = read_base(
            linux_source.archive, source_lock, {"drivers/common.c", "drivers/alpha.c"}
        )
        assert (original["drivers/common.c"].contents) == (b"int common = 1;\n")
        assert ("drivers/alpha.c") not in (original)
        assert (linux_source.snapshot(linux_source.cache)) == (before)
        assert (linux_source.fetch.call_count) == (0)

    def test_failed_patch_keeps_completed_source_and_can_be_retried(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """A normal patch error never publishes partial source or a successful new receipt."""
        source, before = linux_source.prepare("alpha")
        snapshot = linux_source.snapshot(source)
        target = linux_source.root / "targets/beta"
        original = (target / "target.toml").read_text()
        (target / "target.toml").write_text(
            original.replace("patches = []", 'patches = ["bad.patch"]')
        )
        patch = target / "bad.patch"
        patch.write_bytes((PATCHES / "missing.patch").read_bytes())
        with pytest.raises(subprocess.CalledProcessError):
            linux_source.prepare("alpha")
        assert (linux_source.snapshot(source)) == (snapshot)
        linux_state.require_prepared_linux(source, before)
        patch.write_bytes((PATCHES / "common.patch").read_bytes())
        retried, after = linux_source.prepare("alpha")
        assert (retried) == (source)
        assert ((source / "drivers/common.c").read_text()) == ("int common = 2;\n")
        linux_state.require_prepared_linux(source, after)
        assert (linux_source.fetch.call_count) == (1)
