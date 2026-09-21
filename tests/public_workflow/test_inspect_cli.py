# SPDX-License-Identifier: GPL-2.0-only
"""Inspect real synthetic bundles, ZIPs and multi-stream APKs through the public CLI."""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.bundle_state import (
    create_bundle_staging,
    publish_bundle_generation,
    publish_current_bundle,
)

from tests.bundle_support import file_record
from tests.cli_support import prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"


class InspectCliTests(unittest.TestCase):
    """Use the actual archive libraries and resolver; no Kern or phone is involved."""

    def setUp(self) -> None:
        """Keep every inspected artifact and process inside a disposable checkout."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        prepare_cli_checkout(self.root)

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Bound the public subprocess and retain its observable output."""
        return run_process(
            [str(self.root / "fplinux"), *arguments],
            name="public artifact inspection",
            cwd=self.root,
            timeout=10,
        )

    def make_bundle(self, generation: str, profile: str | None = None) -> Path:
        """Publish a normal fixture generation with independently described payload bytes."""
        output = self.root / ".cache/out"
        output.mkdir(parents=True, exist_ok=True)
        staging = create_bundle_staging(output, "example", profile)
        payload = staging / "payload.bin"
        payload.write_bytes(b"abc")
        manifest: dict[str, object] = {
            "target": "example",
            "profile": profile,
            "generation": generation,
            "files": {"payload.bin": file_record(payload)},
            "workspace_digest": "a" * 64,
            "container_image_recipe": "b" * 64,
            "container_image_generation": "c" * 64,
            "apk_signing_key": "d" * 64,
            "device_identity": "e" * 64,
            "linux_recipe": "f" * 64,
            "rootfs_receipt": {},
            "kbuild_receipt": {},
            "boot_artifacts": {"required": []},
        }
        (staging / "build-manifest.json").write_text(json.dumps(manifest))
        return publish_bundle_generation(output, "example", staging, generation, profile)

    def test_bundle_uses_current_pointer_and_requested_profile(self) -> None:
        """A newer unselected generation must not replace the published selection."""
        output = self.root / ".cache/out"
        selected = self.make_bundle("1" * 64)
        publish_current_bundle(output, "example", selected)
        self.make_bundle("2" * 64)
        card = self.make_bundle("3" * 64, "microsd-uboot")
        publish_current_bundle(output, "example", card, "microsd-uboot")
        for arguments, generation, profile in (
            ((), "1" * 64, "default"),
            (("--profile", "microsd-uboot"), "3" * 64, "microsd-uboot"),
        ):
            with self.subTest(profile=profile):
                result = self.run_cli("inspect", "bundle", "example", *arguments)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"generation: {generation}\n", result.stdout)
                self.assertIn(f"profile: {profile}\n", result.stdout)
                self.assertIn(f"3 {ABC_SHA256} payload.bin\n", result.stdout)
                self.assertIn("build-manifest checksums: OK", result.stdout)
                self.assertNotIn(str(self.root), result.stdout)

    def make_archive(
        self, *, candidate: bool = True, damaged: bool = False, extra: bool = False
    ) -> Path:
        """Write an independently checksummed ZIP without calling the production packager."""
        contents = {
            "build-manifest.json": json.dumps(
                {"target": "example", "profile": None, "generation": "1" * 64}
            ).encode(),
            "payload.bin": b"abc",
        }
        if candidate:
            contents["CANDIDATE-NOTICE.txt"] = b"Candidate\n"
        checksums = "".join(
            f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in contents.items()
        )
        if damaged:
            contents["payload.bin"] = b"xyz"
        if extra:
            contents["unexpected.txt"] = b"unlisted file\n"
        path = self.root / "example.zip"
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in contents.items():
                archive.writestr("bundle/" + name, data)
            archive.writestr("bundle/SHA256SUMS", checksums)
        return path

    def test_archive_reports_identity_and_checks_every_listed_file_without_extraction(
        self,
    ) -> None:
        """Both package kinds report verified bytes without creating an extracted tree."""
        for candidate, kind in ((True, "candidate"), (False, "release")):
            with self.subTest(kind=kind):
                archive = self.make_archive(candidate=candidate)
                original = archive.read_bytes()
                result = self.run_cli("inspect", "archive", archive.name)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"kind: {kind}\n", result.stdout)
                self.assertIn("target: example\n", result.stdout)
                self.assertIn(f"3 {ABC_SHA256} payload.bin\n", result.stdout)
                self.assertIn("SHA256SUMS: OK", result.stdout)
                self.assertFalse((self.root / "bundle").exists())
                self.assertFalse((self.root / ".cache").exists())
                self.assertEqual(archive.read_bytes(), original)

    def test_archive_rejects_changed_bytes_and_unlisted_files(self) -> None:
        """Reject modified payloads and incomplete checksum inventories."""
        for damaged, extra, error in (
            (True, False, "checksum mismatch"),
            (False, True, "inventory"),
        ):
            with self.subTest(error=error):
                archive = self.make_archive(damaged=damaged, extra=extra)
                result = self.run_cli("inspect", "archive", archive.name)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(error, result.stderr)
                self.assertNotIn("SHA256SUMS: OK", result.stdout)
                self.assertNotIn("Traceback", result.stderr)

    def test_apk_reads_all_gzip_sections_metadata_and_symlinks_without_installing(self) -> None:
        """Read signature/control/data streams without executing maintainer scripts."""
        parts = []
        for entries in (
            {".SIGN.RSA.example.pub": b"test signature"},
            {
                ".PKGINFO": (
                    b"pkgname = example\npkgver = 1-r0\narch = armv7\n"
                    b"depend = alpha\ndepend = beta\n"
                ),
                ".post-install": b"exit 99\n",
            },
            {"usr/bin/example": b"abc", "usr/bin/link": None},
        ):
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode="w") as archive:
                for name, data in entries.items():
                    member = tarfile.TarInfo(name)
                    if data is None:
                        member.type = tarfile.SYMTYPE
                        member.linkname = "example"
                        archive.addfile(member)
                    else:
                        member.size = len(data)
                        archive.addfile(member, io.BytesIO(data))
            parts.append(gzip.compress(stream.getvalue(), mtime=0))
        path = self.root / "example.apk"
        path.write_bytes(b"".join(parts))
        result = self.run_cli("inspect", "apk", path.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        for expected in (
            "pkgname = example",
            "depend = alpha\ndepend = beta",
            ".post-install",
            "file 3 usr/bin/example",
            "symlink 0 usr/bin/link -> example",
        ):
            self.assertIn(expected, result.stdout)
        self.assertFalse((self.root / "usr").exists())
        self.assertFalse((self.root / ".cache").exists())

    def test_bad_inputs_fail_without_traceback_or_runtime_setup(self) -> None:
        """Missing and unreadable archive formats produce ordinary CLI errors."""
        (self.root / "invalid").write_bytes(b"not an archive")
        for kind in ("archive", "apk"):
            for name in ("missing", "invalid"):
                with self.subTest(kind=kind, name=name):
                    result = self.run_cli("inspect", kind, name)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertNotIn("Traceback", result.stderr)
        self.assertFalse((self.root / ".cache").exists())

    def test_json_is_not_a_public_output_mode(self) -> None:
        """Unsupported output flags fail at argument parsing, before actions can begin."""
        for arguments in (
            ("prune",),
            ("logs", "list"),
            ("inspect", "bundle", "example"),
            ("inspect", "archive", "example.zip"),
            ("inspect", "apk", "example.apk"),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_cli(*arguments, "--json")
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("unrecognized arguments: --json", result.stderr)
                self.assertEqual(result.stdout, "")
        self.assertFalse((self.root / ".cache").exists())


if __name__ == "__main__":
    unittest.main()
