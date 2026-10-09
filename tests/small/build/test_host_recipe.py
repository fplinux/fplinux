# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for declarative host-tool source projections."""

from __future__ import annotations

import hashlib
import struct
import subprocess
import tarfile
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build import host as host_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.environment import downloads
from fplinux_cli.manifests import platforms
from fplinux_cli.manifests import values as manifest_values

if TYPE_CHECKING:
    from types import ModuleType


class HostRecipeTests:
    """Keep local host changes explicit and unable to replace pinned source."""

    @pytest.mark.parametrize("owner", [downloads, manifest_values], ids=("transfer", "sha"))
    def test_host_receipt_misses_after_a_download_owner_changes(
        self, owner: ModuleType, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """Owner edits revoke a saved binary while unrelated documentation keeps it reusable."""
        assert owner.__file__ is not None
        owner_path = Path(owner.__file__)
        captured_owner = tmp_path / owner_path.name
        captured_owner.write_bytes(owner_path.read_bytes())
        monkeypatch.setattr(owner, "__file__", str(captured_owner))
        monkeypatch.setattr(common, "ROOT", tmp_path)
        monkeypatch.setenv("FPLINUX_CONTAINER_IMAGE_SOURCE_RECIPE", "1" * 64)
        monkeypatch.setenv("FPLINUX_CONTAINER_IMAGE_CONTENT", "2" * 64)
        for relative in ("loader.c", "lib/fplinux/fplinux-cli.c", "include/fplinux/fplinux-cli.h"):
            path = tmp_path / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"host input\n")
        recipe = {
            "type": "cc-libusb",
            "name": "loader",
            "source": "loader.c",
            "self_test": False,
        }
        baseline = host_build._host_tool_recipe(  # noqa: SLF001 -- real receipt identity.
            {}, recipe
        )

        # ELF inspection is external; this fixture tests receipt reuse, without compilation.
        binary = tmp_path / "loader"
        header = bytearray(64)
        header[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", header, 18, 62)
        struct.pack_into("<H", header, 54, 56)
        binary.write_bytes(header)
        slot = tmp_path / "slot"
        host_build._publish_host_tool(  # noqa: SLF001 -- real receipt publication.
            slot, baseline, binary
        )
        inspected = subprocess.CompletedProcess(["readelf"], 0, "No dynamic section\n", "")
        with mock.patch.object(subprocess, "run", return_value=inspected):
            output = tmp_path / "output"
            cached = host_build._cached_host_tool(  # noqa: SLF001 -- real cache admission.
                slot, baseline, recipe, output
            )
            assert cached is not None
            assert cached.read_bytes() == bytes(header)
            (tmp_path / "README.md").write_bytes(b"unrelated documentation\n")
            unrelated = host_build._host_tool_recipe(  # noqa: SLF001 -- real receipt identity.
                {}, recipe
            )
            reused = host_build._cached_host_tool(  # noqa: SLF001 -- real cache admission.
                slot, unrelated, recipe, output
            )
            assert reused is not None
            captured_owner.write_bytes(captured_owner.read_bytes() + b"\n# changed owner\n")
            changed = host_build._host_tool_recipe(  # noqa: SLF001 -- real receipt identity.
                {}, recipe
            )
            missed = host_build._cached_host_tool(  # noqa: SLF001 -- real cache admission.
                slot, changed, recipe, output
            )
            assert missed is None

    @staticmethod
    def make_archive_recipe() -> dict[str, object]:
        """Return one complete host archive recipe with a local patch closure."""
        return {
            "type": "make-archive",
            "name": "bridge",
            "source_lock": "bridge-source",
            "cache_name": "bridge.tar.gz",
            "archive_prefix": "bridge-{commit}/",
            "source_directory": "bridge",
            "binary": "bridge",
            "link": "static-libusb",
            "members": [{"path": "bridge.c", "digest_key": "bridge_c_sha256"}],
            "copies": [{"source": "platforms/demo/local-input.h", "destination": "local-input.h"}],
            "patches": ["platforms/demo/local.patch"],
            "self_test": True,
        }

    @pytest.mark.parametrize("field", ["copies", "patches", "self_test"])
    def test_make_archive_declares_local_copies_patches_and_self_test(self, field: str) -> None:
        """A host binary cannot silently omit a project-owned source input or self-test."""
        recipe = self.make_archive_recipe()

        assert (platforms.validate_host_tool(recipe, 0)) == (recipe)
        incomplete = dict(recipe)
        del incomplete[field]
        with pytest.raises(SystemExit, match="must contain exactly"):
            platforms.validate_host_tool(incomplete, 0)

    @staticmethod
    def test_project_copy_cannot_replace_an_existing_projected_file(tmp_path: Path) -> None:
        """A second project copy cannot replace an existing file in the projection."""
        root = tmp_path
        source = root / "upstream"
        source.mkdir()
        local_input = root / "platforms/demo/local-input.h"
        local_input.parent.mkdir(parents=True)
        local_input.write_bytes(b"local input\n")

        with mock.patch.object(common, "ROOT", root):
            host_build.copy_host_project_files(
                source,
                [{"source": "platforms/demo/local-input.h", "destination": "local-input.h"}],
            )
            assert ((source / "local-input.h").read_bytes()) == (b"local input\n")
            with pytest.raises(SystemExit, match="collides with verified source"):
                host_build.copy_host_project_files(
                    source,
                    [
                        {
                            "source": "platforms/demo/local-input.h",
                            "destination": "local-input.h",
                        }
                    ],
                )

    def test_repeated_projection_starts_fresh_and_requests_self_test(self, tmp_path: Path) -> None:
        """With command stubs, each projection starts clean and requests --self-test."""
        root = tmp_path
        archive = root / "bridge.tar.gz"
        archived_source = root / "archive-source.c"
        archived_source.write_bytes(b"upstream\n")
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(archived_source, arcname="bridge-deadbeef/bridge.c")
        local_input = root / "platforms/demo/local-input.h"
        local_input.parent.mkdir(parents=True)
        local_input.write_bytes(b"local input\n")
        work = root / "work"
        output = root / "output"
        work.mkdir()
        recipe = self.make_archive_recipe()
        recipe["patches"] = []
        sources = {
            "bridge-source": {
                "archive_url": "https://example.invalid/bridge.tar.gz",
                "archive_sha256": "0" * 64,
                "commit": "deadbeef",
                "files": {
                    "bridge_c_sha256": hashlib.sha256(b"upstream\n").hexdigest(),
                },
            }
        }

        def build_binary(command: list[str], **_kwargs: object) -> None:
            if command[0] == "make":
                source = Path(command[2])
                (source / "bridge").write_bytes(
                    (source / "bridge.c").read_bytes() + (source / "local-input.h").read_bytes()
                )
                return
            assert (command[1:]) == (["--self-test"])
            assert Path(command[0]).is_file()

        with (
            mock.patch.object(common, "ROOT", root),
            mock.patch.object(sources_build, "fetch", return_value=archive),
            mock.patch.object(process_build, "run", side_effect=build_binary) as run,
        ):
            first = host_build.build_make_host_tool(sources, recipe, work, output)
            second = host_build.build_make_host_tool(sources, recipe, work, output)

        assert (first) == (second)
        assert (second.read_bytes()) == (b"upstream\nlocal input\n")
        assert (run.call_count) == (4)
