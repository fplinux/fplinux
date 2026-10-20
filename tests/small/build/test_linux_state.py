# SPDX-License-Identifier: GPL-2.0-only
"""Exercise shared Linux preparation with real small archives and projections."""

from __future__ import annotations

from dataclasses import replace
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build.kernel import state as linux_state
from fplinux_cli.manifests.linux import discover_linux_targets
from fplinux_cli.workspace import build_inputs as workspace_inputs
from fplinux_cli.workspace import staging as workspace_staging

from tests import ROOT
from tests.fixtures.linux_source import (
    linux_source as linux_source,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.small.workspace.workspace_fixtures import (
    empty_package_graph,
    workspace_root,
    write_registration_source,
)

if TYPE_CHECKING:
    from pathlib import Path

    from tests.fixtures.linux_source import LinuxSourceContext


class SharedLinuxTests:
    """Source switches reuse one tree while causal image inputs remain independent."""

    def test_captured_peer_bytes_and_modes_reach_source_preparation(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """Staging keeps immutable peer inputs while independent bytes retain a recipe hit."""
        root = linux_source.root
        for relative in (
            "platforms/soc/linux/overlay/soc.h",
            "targets/alpha/linux/overlay/alpha.h",
            "targets/beta/linux/overlay/beta.h",
        ):
            (root / relative).write_bytes(b"/* fixture overlay */\n")
        platform_path = root / "platforms/soc/platform.toml"
        with platform_path.open("a") as manifest:
            manifest.write('\n[bootstrap]\nbackend = "sc9820e"\n[host]\ntools = []\n')
        source_lock = root / "sources.lock.toml"
        source_lock.write_text(
            '[linux]\nversion = "test"\nurl = "https://example.invalid/linux.tar.xz"\n'
            f'sha256 = "{linux_source.sources["linux"]["sha256"]}"\n'
            + (ROOT / "tests/fixtures/sc9820e-manifest/sources.lock.toml").read_text()
        )
        with (root / "targets/alpha/target.toml").open("a") as manifest:
            manifest.write(
                '\n[loader.build]\nfdl1_source = "chipram"\n'
                'fdl2_source = "u_boot"\nsml_source = "trusted_firmware"\n'
            )
        selected = discover_linux_targets(
            root, linux_source.sources, linux_source.sources["linux"]["sha256"]
        )[0]
        selected.config["device_data"] = {"groups": {}}
        selected.config["linux"]["root"] = {"kind": "initramfs"}
        selected_files = [
            (path.relative_to(root).as_posix(), path)
            for path in (
                source_lock,
                platform_path,
                root / "targets/alpha/target.toml",
                root / "targets/alpha/driver.c",
                root / "targets/alpha/fragment",
            )
        ]
        selected_files.append(write_registration_source(root))
        with (
            workspace_root(root),
            empty_package_graph(),
            mock.patch.object(workspace_inputs, "load_target", return_value=selected.config),
            mock.patch.object(workspace_inputs, "load_platform", return_value={}),
            mock.patch.object(
                workspace_inputs, "target_build_source_files", return_value=selected_files
            ),
        ):
            before = workspace_inputs.target_workspace_snapshot("alpha")
            peer = root / "targets/beta/driver.c"
            peer.write_bytes(b"int beta = 2;\n")
            peer.chmod(0o640)
            changed = workspace_inputs.target_workspace_snapshot("alpha")
            peer.write_bytes(b"int beta = 3;\n")
            peer.chmod(0o600)
            before_path = workspace_staging.stage_workspace_snapshot(before)
            changed_path = workspace_staging.stage_workspace_snapshot(changed)

            assert before.recipe == changed.recipe
            assert before_path != changed_path
            staged_peer = changed_path / "targets/beta/driver.c"
            assert staged_peer.read_bytes() == b"int beta = 2;\n"
            assert staged_peer.stat().st_mode & 0o777 == 0o640

            with mock.patch.object(common, "ROOT", before_path):
                source, old_state = replace(linux_source, root=before_path).prepare("alpha")
            assert (source / "drivers/beta.c").read_bytes() == b"int beta = 1;\n"
            with mock.patch.object(common, "ROOT", changed_path):
                _, changed_state = replace(linux_source, root=changed_path).prepare("alpha")
            assert (source / "drivers/beta.c").read_bytes() == b"int beta = 2;\n"
            assert changed_state.linux_recipe == old_state.linux_recipe
            assert changed_state.tree_recipe != old_state.tree_recipe

            unrelated = root / "targets/beta/rootfs/config"
            unrelated.parent.mkdir()
            unrelated.write_bytes(b"unrelated rootfs input\n")
            current = workspace_inputs.target_workspace_snapshot("alpha")
            unrelated.write_bytes(b"changed unrelated rootfs input\n")
            assert workspace_inputs.target_workspace_snapshot("alpha").recipe == current.recipe

    def test_target_patch_runs_after_its_board_copy(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """The normalized declaration reader retains the builder's copy-before-patch order."""
        manifest = linux_source.root / "targets/alpha/target.toml"
        manifest.write_text(
            manifest.read_text().replace("patches = []", 'patches = ["driver.patch"]')
        )
        (manifest.parent / "driver.patch").write_text(
            "--- a/drivers/alpha.c\n+++ b/drivers/alpha.c\n"
            "@@ -1 +1 @@\n-int alpha = 1;\n+int alpha = 2;\n"
        )

        source, _ = linux_source.prepare("alpha")

        assert (source / "drivers/alpha.c").read_bytes() == b"int alpha = 2;\n"

    def test_peer_overlay_add_edit_remove_updates_tree_and_preserves_image_independence(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """Peer topology is shared, while only its image depends on independent driver bytes."""
        source, original = linux_source.prepare("alpha")
        added_driver = linux_source.root / "targets/beta/linux/overlay/drivers/nested/extra.c"
        added_driver.parent.mkdir(parents=True)
        added_driver.write_text("int extra = 1;\n")

        _, added = linux_source.prepare("alpha")
        _, beta_before = linux_source.prepare("beta")
        assert (source / "drivers/nested/extra.c").read_text() == "int extra = 1;\n"
        assert added.linux_recipe != original.linux_recipe

        added_driver.write_text("int extra = 2;\n")
        _, edited = linux_source.prepare("alpha")
        _, beta_after = linux_source.prepare("beta")
        assert (source / "drivers/nested/extra.c").read_text() == "int extra = 2;\n"
        assert edited.linux_recipe == added.linux_recipe
        assert beta_after.linux_recipe != beta_before.linux_recipe
        assert edited.tree_recipe != added.tree_recipe

        added_driver.unlink()
        _, removed = linux_source.prepare("alpha")
        assert not (source / "drivers/nested/extra.c").exists()
        assert removed.linux_recipe == original.linux_recipe
        assert linux_source.fetch.call_count == 1

    @pytest.mark.parametrize("destination", ["drivers/alpha.c", "drivers/beta.c"])
    def test_overlay_copy_owner_conflicts_fail_before_source_publication(
        self, linux_source: LinuxSourceContext, destination: str
    ) -> None:
        """Overlay files cannot replace another owner or an explicit copy from the same board."""
        conflicting = linux_source.root / "targets/beta/linux/overlay" / destination
        conflicting.parent.mkdir(parents=True)
        conflicting.write_text("int conflicting = 1;\n")

        with pytest.raises(SystemExit, match="multiple owners"):
            linux_source.prepare("alpha")

        assert not (linux_source.cache / "linux/sources").exists()

    def test_platform_overlay_and_foreign_header_copies_both_remain_causal(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """One shared header owner can coexist with platform overlay files without duplication."""
        manifest = linux_source.root / "platforms/soc/platform.toml"
        manifest.write_text(
            manifest.read_text().replace("copies = []\n", "")
            + '\n[[linux.copies]]\nsource = "shared/foreign.h"\n'
            'destination = "include/foreign.h"\n'
        )
        foreign = linux_source.root / "shared/foreign.h"
        foreign.parent.mkdir()
        foreign.write_text("#define FOREIGN 1\n")
        owned = linux_source.root / "platforms/soc/linux/overlay/include/owned.h"
        owned.parent.mkdir()
        owned.write_text("#define OWNED 1\n")
        source, before = linux_source.prepare("alpha")
        assert (source / "include/foreign.h").read_text() == "#define FOREIGN 1\n"
        assert (source / "include/owned.h").read_text() == "#define OWNED 1\n"

        owned.write_text("#define OWNED 2\n")
        _, after_owned = linux_source.prepare("alpha")
        assert after_owned.linux_recipe != before.linux_recipe
        assert (source / "include/owned.h").read_text() == "#define OWNED 2\n"
        foreign.write_text("#define FOREIGN 2\n")
        _, after_foreign = linux_source.prepare("alpha")
        assert after_foreign.linux_recipe != after_owned.linux_recipe
        assert (source / "include/foreign.h").read_text() == "#define FOREIGN 2\n"

    def test_board_and_profile_switches_leave_shared_source_unchanged(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """A/B/A and profile switches need neither another archive nor source writes."""
        source, first = linux_source.prepare("alpha")
        before = linux_source.snapshot(source)
        linux_source.archive.unlink()
        for name, external in (("beta", False), ("alpha", True), ("alpha", False)):
            current, state = linux_source.prepare(name, external=external)
            assert (current) == (source)
            assert (linux_source.snapshot(source)) == (before)
            linux_state.require_prepared_linux(source, state)
            if name == "alpha" and external:
                assert (state.linux_recipe) != (first.linux_recipe)
        assert (linux_source.fetch.call_count) == (1)
        assert (len(tuple((linux_source.cache / "linux/sources").iterdir()))) == (1)
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

    def test_peer_driver_edit_changes_only_its_destination_and_own_recipe(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """An independent board edit preserves other boards' build identities and source mtimes."""
        source, alpha_before = linux_source.prepare("alpha")
        _, beta_before = linux_source.prepare("beta")
        before = linux_source.snapshot(source)
        (linux_source.root / "targets/beta/driver.c").write_text("int beta = 2;\n")
        _, alpha_after = linux_source.prepare("alpha")
        _, beta_after = linux_source.prepare("beta")
        after = linux_source.snapshot(source)
        changes = {
            name
            for name in before
            if before[name] != after[name] and not name.startswith(".fplinux-")
        }
        assert (changes) == ({"drivers/beta.c"})
        assert (alpha_after.linux_recipe) == (alpha_before.linux_recipe)
        assert (beta_after.linux_recipe) != (beta_before.linux_recipe)
        assert (alpha_after.tree_recipe) != (alpha_before.tree_recipe)
        assert (linux_source.fetch.call_count) == (1)
        with pytest.raises(linux_state.LinuxStateError, match="changed after preparation"):
            linux_state.require_prepared_linux(source, alpha_before)

    def test_source_update_failure_leaves_consumers_invalid_until_reprepared(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """A filesystem-write failure cannot retain or publish a successful tree receipt."""
        source, before = linux_source.prepare("alpha")
        (linux_source.root / "targets/alpha/driver.c").write_text("int alpha = 2;\n")
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
            linux_source.prepare("alpha")
        with pytest.raises(linux_state.LinuxStateError, match="changed after preparation"):
            linux_state.require_prepared_linux(source, before)

        _, after = linux_source.prepare("alpha")
        assert (after.tree_recipe) != (before.tree_recipe)
        assert ((source / "drivers/alpha.c").read_text()) == ("int alpha = 2;\n")
        linux_state.require_prepared_linux(source, after)

    def test_two_platforms_share_source_and_another_base_uses_its_own_slot(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """Source identity, not platform identity, chooses the complete Linux tree."""
        linux_source.platform("other")
        linux_source.target("delta", platform="other")
        source, _ = linux_source.prepare("alpha")
        other, _ = linux_source.prepare("delta")
        assert (other) == (source)
        assert (source / "arch/other/boot/dts/fplinux-delta-identity.dtsi").is_file()
        with linux_source.archive.open("ab") as archive:
            archive.write(b"\0")
        linux_source.sources["linux"]["sha256"] = common.sha256_file(linux_source.archive)
        changed, _ = linux_source.prepare("alpha")
        assert (changed) != (source)
        assert (source / "drivers/alpha.c").is_file()
        assert (linux_source.fetch.call_count) == (2)

    def test_other_platform_append_to_common_code_changes_selected_recipe(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """Platform appends to shared upstream code remain causal for every consumer."""
        linux_source.platform("other")
        linux_source.target("delta", platform="other")
        directory = linux_source.root / "platforms/other"
        manifest = directory / "platform.toml"
        manifest.write_text(
            manifest.read_text().replace("appends = []\n", "")
            + '\n[[linux.appends]]\nsource = "platforms/other/common.fragment"\n'
            'destination = "drivers/common.c"\n'
        )
        fragment = directory / "common.fragment"
        fragment.write_text("int other = 1;\n")
        source, before = linux_source.prepare("alpha")
        fragment.write_text("int other = 2;\n")
        _, after = linux_source.prepare("alpha")
        assert ((source / "drivers/common.c").read_text()) == (
            "int common = 1;\n\nint other = 2;\n"
        )
        assert (after.linux_recipe) != (before.linux_recipe)

    def test_peer_board_fragment_change_invalidates_shared_config_recipe(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """The shared Kconfig input cannot hide a peer fragment's changed defaults."""
        source, before = linux_source.prepare("alpha")
        (linux_source.root / "targets/beta/fragment").write_text(
            'config BETA\n\tbool "beta"\n\tdefault y\n'
        )
        _, after = linux_source.prepare("alpha")
        assert ("\tdefault y\n") in ((source / "drivers/Kconfig").read_text())
        assert (after.linux_recipe) != (before.linux_recipe)

    def test_conflicting_copy_owners_fail_before_source_publication(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """Discovery order cannot silently choose between different board source owners."""
        path = linux_source.root / "targets/beta/target.toml"
        path.write_text(path.read_text().replace("drivers/beta.c", "drivers/alpha.c"))
        with pytest.raises(SystemExit, match=r"multiple owners: drivers/alpha.c"):
            linux_source.prepare("alpha")
        assert not ((linux_source.cache / "linux/sources").exists())

    def test_board_copy_cannot_replace_an_upstream_file(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """An independent board copy cannot bypass shared-code causal tracking."""
        source, prepared = linux_source.prepare("alpha")
        before = linux_source.snapshot(source)
        manifest = linux_source.root / "targets/beta/target.toml"
        manifest.write_text(manifest.read_text().replace("drivers/beta.c", "drivers/common.c"))
        with pytest.raises(SystemExit, match="target Linux copy must add a new board file"):
            linux_source.prepare("alpha")
        assert (linux_source.snapshot(source)) == (before)
        assert ((source / "drivers/common.c").read_text()) == ("int common = 1;\n")
        linux_state.require_prepared_linux(source, prepared)

    def test_peer_platform_new_header_contents_and_destination_are_causal(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """Shared copied headers invalidate consumers even when upstream had no such path."""
        linux_source.platform("other")
        linux_source.target("delta", platform="other")
        directory = linux_source.root / "platforms/other"
        manifest = directory / "platform.toml"
        manifest.write_text(
            manifest.read_text().replace("copies = []\n", "")
            + '\n[[linux.copies]]\nsource = "platforms/other/shared.h"\n'
            'destination = "include/shared.h"\n'
        )
        header = directory / "shared.h"
        header.write_text("#define SHARED_VALUE 1\n")
        (linux_source.root / "targets/alpha/driver.c").write_text(
            "#include <shared.h>\nint alpha = SHARED_VALUE;\n"
        )
        source, before = linux_source.prepare("alpha")
        header.write_text("#define SHARED_VALUE 2\n")
        _, changed = linux_source.prepare("alpha")
        assert ((source / "include/shared.h").read_text()) == ("#define SHARED_VALUE 2\n")
        assert (changed.linux_recipe) != (before.linux_recipe)
        manifest.write_text(
            manifest.read_text().replace("include/shared.h", "include/moved-shared.h")
        )
        _, moved = linux_source.prepare("alpha")
        assert not ((source / "include/shared.h").exists())
        assert ((source / "include/moved-shared.h").read_text()) == ("#define SHARED_VALUE 2\n")
        assert (moved.linux_recipe) != (changed.linux_recipe)

    def test_profile_root_is_written_only_to_build_output_and_keeps_mtime_on_reuse(
        self, linux_source: LinuxSourceContext
    ) -> None:
        """Separate build directories retain their own root arguments without source mutation."""
        source, _ = linux_source.prepare("alpha")
        before = linux_source.snapshot(source)
        config = {"linux": {"root": {"kind": "initramfs"}}}
        output = linux_source.root / "out/default"
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
        second = linux_state.write_profile_root(linux_source.root / "out/sd", external)
        assert (b"root=PARTUUID=12345678-02 rootfstype=ext4 rootwait=5 rw") in (
            second.read_bytes()
        )
        assert (b"rdinit=/init") in (path.read_bytes())
        assert (linux_source.snapshot(source)) == (before)
