# SPDX-License-Identifier: GPL-2.0-only
"""Exercise shared Linux preparation with real small archives and projections."""

from __future__ import annotations

from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build.kernel import state as linux_state

from tests.fixtures.linux_source import LinuxSourceFixture

if TYPE_CHECKING:
    from pathlib import Path


class SharedLinuxTests(LinuxSourceFixture):
    """Source switches reuse one tree while causal image inputs remain independent."""

    def test_board_and_profile_switches_leave_shared_source_unchanged(self) -> None:
        """A/B/A and profile switches need neither another archive nor source writes."""
        source, first = self.prepare("alpha")
        before = self.snapshot(source)
        self.archive.unlink()
        for name, external in (("beta", False), ("alpha", True), ("alpha", False)):
            current, state = self.prepare(name, external=external)
            assert (current) == (source)
            assert (self.snapshot(source)) == (before)
            linux_state.require_prepared_linux(source, state)
            if name == "alpha" and external:
                assert (state.linux_recipe) != (first.linux_recipe)
        assert (self.fetch.call_count) == (1)
        assert (len(tuple((self.cache / "linux/sources").iterdir()))) == (1)
        assert ((source / "drivers/alpha.c").read_text()) == ("int alpha = 1;\n")
        assert ((source / "drivers/beta.c").read_text()) == ("int beta = 1;\n")
        assert ((source / "drivers/Kconfig").read_text()) == (
            'menu "Drivers"\nendmenu\n\nconfig ALPHA\n\tbool "alpha"\n'
            '\nconfig BETA\n\tbool "beta"\n'
        )
        assert (b"Example alpha") in (
            (source / "arch/soc/boot/dts/fplinux-alpha-identity.dtsi").read_bytes()
        )
        assert (b"Example beta") in (
            (source / "arch/soc/boot/dts/fplinux-beta-identity.dtsi").read_bytes()
        )
        assert not ((source / "arch/soc/boot/dts/fplinux-root.dtsi").exists())

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
        assert (changes) == ({"drivers/beta.c"})
        assert (alpha_after.linux_recipe) == (alpha_before.linux_recipe)
        assert (beta_after.linux_recipe) != (beta_before.linux_recipe)
        assert (alpha_after.tree_recipe) != (alpha_before.tree_recipe)
        assert (self.fetch.call_count) == (1)
        with pytest.raises(linux_state.LinuxStateError, match="changed after preparation"):
            linux_state.require_prepared_linux(source, alpha_before)

    def test_source_update_failure_leaves_consumers_invalid_until_reprepared(self) -> None:
        """A filesystem-write failure cannot retain or publish a successful tree receipt."""
        source, before = self.prepare("alpha")
        (self.root / "targets/alpha/driver.c").write_text("int alpha = 2;\n")
        write_changed_file = linux_state.write_changed_file

        def fail_driver_write(path: Path, contents: bytes, mode: int) -> None:
            assert (linux_state.inspect_prepared_linux(source, before)) is None
            if path == source / "drivers/alpha.c":
                message = "controlled source write failure"
                raise OSError(message)
            write_changed_file(path, contents, mode)

        with (
            mock.patch.object(linux_state, "write_changed_file", side_effect=fail_driver_write),
            pytest.raises(OSError, match="controlled source write failure"),
        ):
            self.prepare("alpha")
        with pytest.raises(linux_state.LinuxStateError, match="changed after preparation"):
            linux_state.require_prepared_linux(source, before)

        _, after = self.prepare("alpha")
        assert (after.tree_recipe) != (before.tree_recipe)
        assert ((source / "drivers/alpha.c").read_text()) == ("int alpha = 2;\n")
        linux_state.require_prepared_linux(source, after)

    def test_two_platforms_share_source_and_another_base_uses_its_own_slot(self) -> None:
        """Source identity, not platform identity, chooses the complete Linux tree."""
        self.platform("other")
        self.target("delta", platform="other")
        source, _ = self.prepare("alpha")
        other, _ = self.prepare("delta")
        assert (other) == (source)
        assert (source / "arch/other/boot/dts/fplinux-delta-identity.dtsi").is_file()
        with self.archive.open("ab") as archive:
            archive.write(b"\0")
        self.sources["linux"]["sha256"] = common.sha256_file(self.archive)
        changed, _ = self.prepare("alpha")
        assert (changed) != (source)
        assert (source / "drivers/alpha.c").is_file()
        assert (self.fetch.call_count) == (2)

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
        assert ((source / "drivers/common.c").read_text()) == (
            "int common = 1;\n\nint other = 2;\n"
        )
        assert (after.linux_recipe) != (before.linux_recipe)

    def test_peer_board_fragment_change_invalidates_shared_config_recipe(self) -> None:
        """The shared Kconfig input cannot hide a peer fragment's changed defaults."""
        source, before = self.prepare("alpha")
        (self.root / "targets/beta/fragment").write_text(
            'config BETA\n\tbool "beta"\n\tdefault y\n'
        )
        _, after = self.prepare("alpha")
        assert ("\tdefault y\n") in ((source / "drivers/Kconfig").read_text())
        assert (after.linux_recipe) != (before.linux_recipe)

    def test_conflicting_copy_owners_fail_before_source_publication(self) -> None:
        """Discovery order cannot silently choose between different board source owners."""
        path = self.root / "targets/beta/target.toml"
        path.write_text(path.read_text().replace("drivers/beta.c", "drivers/alpha.c"))
        with pytest.raises(SystemExit, match=r"multiple owners: drivers/alpha.c"):
            self.prepare("alpha")
        assert not ((self.cache / "linux/sources").exists())

    def test_board_copy_cannot_replace_an_upstream_file(self) -> None:
        """An independent board copy cannot bypass shared-code causal tracking."""
        source, prepared = self.prepare("alpha")
        before = self.snapshot(source)
        manifest = self.root / "targets/beta/target.toml"
        manifest.write_text(manifest.read_text().replace("drivers/beta.c", "drivers/common.c"))
        with pytest.raises(SystemExit, match="target Linux copy must add a new board file"):
            self.prepare("alpha")
        assert (self.snapshot(source)) == (before)
        assert ((source / "drivers/common.c").read_text()) == ("int common = 1;\n")
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
        assert ((source / "include/shared.h").read_text()) == ("#define SHARED_VALUE 2\n")
        assert (changed.linux_recipe) != (before.linux_recipe)
        manifest.write_text(
            manifest.read_text().replace("include/shared.h", "include/moved-shared.h")
        )
        _, moved = self.prepare("alpha")
        assert not ((source / "include/shared.h").exists())
        assert ((source / "include/moved-shared.h").read_text()) == ("#define SHARED_VALUE 2\n")
        assert (moved.linux_recipe) != (changed.linux_recipe)

    def test_profile_root_is_written_only_to_build_output_and_keeps_mtime_on_reuse(self) -> None:
        """Separate build directories retain their own root arguments without source mutation."""
        source, _ = self.prepare("alpha")
        before = self.snapshot(source)
        config = {"linux": {"root": {"kind": "initramfs"}}}
        output = self.root / "out/default"
        path = linux_state.write_profile_root(output, config)
        first = path.stat()
        assert (b"rdinit=/init") in (path.read_bytes())
        assert (linux_state.write_profile_root(output, config).stat().st_mtime_ns) == (
            first.st_mtime_ns
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
        assert (b"root=PARTUUID=12345678-02 rootfstype=ext4 rootwait=5 rw") in (
            second.read_bytes()
        )
        assert (b"rdinit=/init") in (path.read_bytes())
        assert (self.snapshot(source)) == (before)
