# SPDX-License-Identifier: GPL-2.0-only
"""Probe output publication with an explicit compiler-process stub."""

from __future__ import annotations

import hashlib
import io
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli import common, output
from fplinux_cli.cli import probe
from fplinux_cli.environment import kern
from fplinux_cli.output import RunReporter, Stage

if TYPE_CHECKING:
    from collections.abc import Sequence


class ProbePublicationTests(unittest.TestCase):
    """Check real file publication; the compiler stub provides no ARM evidence."""

    def test_preparation_extracts_release_keys_from_verified_archive(self) -> None:
        """Probe preparation reuses verified minirootfs bytes and extracts only signing keys."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = io.BytesIO()
            with tarfile.open(fileobj=archive, mode="w:gz") as bundle:
                for name, contents in (
                    ("etc/apk/keys/release.pub", b"release signing key\n"),
                    ("etc/irrelevant", b"not a key\n"),
                ):
                    member = tarfile.TarInfo(name)
                    member.size = len(contents)
                    bundle.addfile(member, io.BytesIO(contents))
            contents = archive.getvalue()
            lock = {
                "release": "example",
                "arch": "armv7",
                "minirootfs": {
                    "url": "https://example.invalid/minirootfs.tar.gz",
                    "sha256": hashlib.sha256(contents).hexdigest(),
                    "bytes": len(contents),
                },
            }
            with (
                mock.patch.object(common, "ROOT", root),
                mock.patch("urllib.request.urlopen", return_value=io.BytesIO(contents)),
            ):
                keys = probe._prepare_keys(lock, root / "first")  # noqa: SLF001
            self.assertEqual((keys / "release.pub").read_bytes(), b"release signing key\n")
            self.assertFalse((root / "first/etc/irrelevant").exists())
            with (
                mock.patch.object(common, "ROOT", root),
                mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
            ):
                keys = probe._prepare_keys(lock, root / "second")  # noqa: SLF001
            self.assertEqual((keys / "release.pub").read_bytes(), b"release signing key\n")

    def test_output_is_replaced_only_after_compilation_succeeds(self) -> None:
        """Partial compiler output must never replace an existing useful executable."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "probe.c"
            source.write_text("int main(void) { return 0; }\n")
            destination = root / ".cache/tools/probe"
            destination.parent.mkdir(parents=True)
            destination.write_bytes(b"previous executable")
            destination.chmod(0o751)

            def compiler_stub(command: Sequence[str], **_kwargs: object) -> None:
                """Write the requested temporary output before reporting compiler status."""
                requested = Path(command[command.index("-o") + 1])
                (destination.parent / requested.name).write_bytes(b"new executable")
                if not succeeds:
                    raise SystemExit(1)

            with (
                mock.patch.object(common, "ROOT", root),
                mock.patch.object(output, "ROOT", root),
                mock.patch.object(kern, "ROOT", root),
                mock.patch.object(Stage, "run", side_effect=compiler_stub),
            ):
                for succeeds in (False, True):
                    with self.subTest(succeeds=succeeds):
                        reporter = RunReporter.create("probe-build", target=None, verbose=False)

                        def compile_probe(run: RunReporter) -> None:
                            probe._compile_probe(  # noqa: SLF001 -- publication boundary.
                                source,
                                destination,
                                root / "sysroot",
                                kern="stubbed-kern",
                                image="stubbed-image",
                                reporter=run,
                            )

                        if succeeds:
                            compile_probe(reporter)
                            self.assertEqual(destination.read_bytes(), b"new executable")
                            self.assertEqual(destination.stat().st_mode & 0o777, 0o755)
                        else:
                            with self.assertRaises(SystemExit):
                                compile_probe(reporter)
                            self.assertEqual(destination.read_bytes(), b"previous executable")
                            self.assertEqual(destination.stat().st_mode & 0o777, 0o751)
                        self.assertEqual(list(destination.parent.iterdir()), [destination])


if __name__ == "__main__":
    unittest.main()
