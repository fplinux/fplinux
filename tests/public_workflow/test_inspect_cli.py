# SPDX-License-Identifier: GPL-2.0-only
"""Inspect real synthetic bundles, ZIPs and multi-stream APKs through the public CLI."""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import tarfile
import zipfile
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.artifacts.bundles import (
    create_bundle_staging,
    publish_bundle_generation,
    publish_current_bundle,
)

from tests.bundle_support import file_record
from tests.cli_support import prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from pathlib import Path

ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"


class InspectCliTests:
    """Use the actual archive libraries and resolver; no Kern or phone is involved."""

    @pytest.fixture(autouse=True)
    def _prepare_inputs(self, tmp_path: Path) -> None:
        """Keep every inspected artifact and process inside a disposable checkout."""
        self.root = tmp_path
        prepare_cli_checkout(self.root)

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Bound the public subprocess and retain its observable output."""
        return run_process(
            [str(self.root / "fplinux"), *arguments],
            name="public artifact inspection",
            cwd=self.root,
            timeout=10,
        )

    def make_bundle(
        self, generation: str, profile: str | None = None, *, build_type: str = "release"
    ) -> Path:
        """Publish a normal fixture generation with independently described payload bytes."""
        output = self.root / ".cache/out"
        output.mkdir(parents=True, exist_ok=True)
        staging = create_bundle_staging(output, "example", profile, build_type=build_type)
        payload = staging / "payload.bin"
        payload.write_bytes(b"abc")
        manifest: dict[str, object] = {
            "target": "example",
            "profile": profile,
            "build_type": build_type,
            "generation": generation,
            "files": {"payload.bin": file_record(payload)},
            "workspace_digest": "a" * 64,
            "container_image_recipe": "b" * 64,
            "container_image_content": "c" * 64,
            "apk_signing_key": "d" * 64,
            "device_identity": "e" * 64,
            "linux_recipe": "f" * 64,
            "rootfs_receipt": {},
            "kbuild_receipt": {},
            "boot_artifacts": {"required": []},
        }
        (staging / "build-manifest.json").write_text(json.dumps(manifest))
        return publish_bundle_generation(
            output, "example", staging, generation, profile, build_type=build_type
        )

    def test_build_type_selection_is_explicit_and_never_falls_back(self) -> None:
        """The public inspector reports only the requested type's current generation."""
        output = self.root / ".cache/out"
        release = self.make_bundle("1" * 64)
        publish_current_bundle(output, "example", release)
        missing = self.run_cli("inspect", "bundle", "example", "--build-type", "debug")
        assert (missing.returncode) != (0)
        assert ("build example --build-type debug") in (missing.stderr)
        debug = self.make_bundle("2" * 64, build_type="debug")
        publish_current_bundle(output, "example", debug, build_type="debug")

        for build_type, generation in (
            ("release", "1" * 64),
            ("debug", "2" * 64),
            ("release", "1" * 64),
        ):
            result = self.run_cli("inspect", "bundle", "example", "--build-type", build_type)
            assert (result.returncode) == (0), result.stderr
            assert (f"build_type: {build_type}") in (result.stdout)
            assert (f"generation: {generation}") in (result.stdout)

    @pytest.mark.parametrize(
        ("arguments", "generation", "profile"),
        [
            pytest.param((), "1" * 64, "default", id="default"),
            pytest.param(("--profile", "microsd-uboot"), "3" * 64, "microsd-uboot", id="microsd"),
        ],
    )
    def test_bundle_uses_current_pointer_and_requested_profile(
        self, arguments: tuple[str, ...], generation: str, profile: str
    ) -> None:
        """A newer unselected generation must not replace the published selection."""
        output = self.root / ".cache/out"
        selected = self.make_bundle("1" * 64)
        publish_current_bundle(output, "example", selected)
        self.make_bundle("2" * 64)
        card = self.make_bundle("3" * 64, "microsd-uboot")
        publish_current_bundle(output, "example", card, "microsd-uboot")
        result = self.run_cli("inspect", "bundle", "example", *arguments)
        assert (result.returncode) == (0), result.stderr
        assert (f"generation: {generation}\n") in (result.stdout)
        assert (f"profile: {profile}\n") in (result.stdout)
        assert (f"3 {ABC_SHA256} payload.bin\n") in (result.stdout)
        assert ("build-manifest checksums: OK") in (result.stdout)
        assert (str(self.root)) not in (result.stdout)

    def make_archive(
        self, *, candidate: bool = True, damaged: bool = False, extra: bool = False
    ) -> Path:
        """Write an independently checksummed ZIP without calling the production packager."""
        contents = {
            "build-manifest.json": json.dumps(
                {
                    "target": "example",
                    "profile": None,
                    "build_type": "release",
                    "generation": "1" * 64,
                }
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

    @pytest.mark.parametrize(
        ("candidate", "kind"),
        [
            pytest.param(True, "candidate", id="candidate"),
            pytest.param(False, "release", id="release"),
        ],
    )
    def test_archive_reports_identity_and_checks_every_listed_file_without_extraction(
        self, *, candidate: bool, kind: str
    ) -> None:
        """Both package kinds report verified bytes without creating an extracted tree."""
        archive = self.make_archive(candidate=candidate)
        original = archive.read_bytes()
        result = self.run_cli("inspect", "archive", archive.name)
        assert (result.returncode) == (0), result.stderr
        assert (f"kind: {kind}\n") in (result.stdout)
        assert ("target: example\n") in (result.stdout)
        assert (f"3 {ABC_SHA256} payload.bin\n") in (result.stdout)
        assert ("SHA256SUMS: OK") in (result.stdout)
        assert not ((self.root / "bundle").exists())
        assert not ((self.root / ".cache").exists())
        assert (archive.read_bytes()) == (original)

    @pytest.mark.parametrize(
        ("damaged", "extra", "error"),
        [
            pytest.param(True, False, "checksum mismatch", id="changed-payload"),
            pytest.param(False, True, "inventory", id="unlisted-file"),
        ],
    )
    def test_archive_rejects_changed_bytes_and_unlisted_files(
        self, *, damaged: bool, extra: bool, error: str
    ) -> None:
        """Reject modified payloads and incomplete checksum inventories."""
        archive = self.make_archive(damaged=damaged, extra=extra)
        result = self.run_cli("inspect", "archive", archive.name)
        assert (result.returncode) == (1), result.stderr
        assert (error) in (result.stderr)
        assert ("SHA256SUMS: OK") not in (result.stdout)
        assert ("Traceback") not in (result.stderr)

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
        assert (result.returncode) == (0), result.stderr
        for expected in (
            "pkgname = example",
            "depend = alpha\ndepend = beta",
            ".post-install",
            "file 3 usr/bin/example",
            "symlink 0 usr/bin/link -> example",
        ):
            assert (expected) in (result.stdout)
        assert not ((self.root / "usr").exists())
        assert not ((self.root / ".cache").exists())

    @pytest.mark.parametrize(
        ("kind", "name"),
        [
            pytest.param("archive", "missing", id="missing-archive"),
            pytest.param("archive", "invalid", id="invalid-archive"),
            pytest.param("apk", "missing", id="missing-apk"),
            pytest.param("apk", "invalid", id="invalid-apk"),
        ],
    )
    def test_bad_inputs_fail_without_traceback_or_runtime_setup(
        self, kind: str, name: str
    ) -> None:
        """Missing and unreadable archive formats produce ordinary CLI errors."""
        (self.root / "invalid").write_bytes(b"not an archive")
        result = self.run_cli("inspect", kind, name)
        assert (result.returncode) == (1), result.stderr
        assert ("Traceback") not in (result.stderr)
        assert not ((self.root / ".cache").exists())
