# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for declarative host-tool source projections."""

from __future__ import annotations

import hashlib
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import builder, config


class HostRecipeTests(unittest.TestCase):
    """Keep local host changes explicit and unable to replace pinned source."""

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

    def test_make_archive_declares_local_copies_patches_and_self_test(self) -> None:
        """A host binary cannot silently omit a project-owned source input or self-test."""
        recipe = self.make_archive_recipe()

        self.assertEqual(config.validate_host_tool(recipe, 0), recipe)
        for field in ("copies", "patches", "self_test"):
            incomplete = dict(recipe)
            del incomplete[field]
            with (
                self.subTest(field=field),
                self.assertRaisesRegex(SystemExit, "must contain exactly"),
            ):
                config.validate_host_tool(incomplete, 0)

    def test_project_copy_cannot_replace_a_verified_member(self) -> None:
        """An archive projection rejects replacing bytes already verified against the lock."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "upstream"
            source.mkdir()
            local_input = root / "platforms/demo/local-input.h"
            local_input.parent.mkdir(parents=True)
            local_input.write_bytes(b"local input\n")

            with mock.patch.object(builder, "ROOT", root):
                builder.copy_host_project_files(
                    source,
                    [{"source": "platforms/demo/local-input.h", "destination": "local-input.h"}],
                )
                self.assertEqual((source / "local-input.h").read_bytes(), b"local input\n")
                with self.assertRaisesRegex(SystemExit, "collides with verified source"):
                    builder.copy_host_project_files(
                        source,
                        [
                            {
                                "source": "platforms/demo/local-input.h",
                                "destination": "local-input.h",
                            }
                        ],
                    )

    def test_repeated_build_uses_a_fresh_projection_and_runs_self_test(self) -> None:
        """Each rebuild starts clean and self-tests the resulting binary."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
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
                        (source / "bridge.c").read_bytes()
                        + (source / "local-input.h").read_bytes()
                    )
                    return
                self.assertEqual(command[1:], ["--self-test"])
                self.assertTrue(Path(command[0]).is_file())

            with (
                mock.patch.object(builder, "ROOT", root),
                mock.patch.object(builder, "fetch", return_value=archive),
                mock.patch.object(builder, "run", side_effect=build_binary) as run,
            ):
                first = builder.build_make_host_tool(sources, recipe, work, output)
                second = builder.build_make_host_tool(sources, recipe, work, output)

            self.assertEqual(first, second)
            self.assertEqual(second.read_bytes(), b"upstream\nlocal input\n")
            self.assertEqual(run.call_count, 4)


if __name__ == "__main__":
    unittest.main()
