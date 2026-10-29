# SPDX-License-Identifier: GPL-2.0-only
"""Inspect real synthetic bundles, ZIPs and multi-stream APKs through the public CLI."""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import os
import tarfile
import zipfile
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.artifacts.bundles import (
    create_bundle_staging,
    publish_bundle_generation,
    publish_current_bundle,
)

from tests import ROOT
from tests.bundle_support import file_record
from tests.cli_support import prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from pathlib import Path

ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"


def test_site_inputs_inspection_does_not_create_an_empty_data_root(tmp_path: Path) -> None:
    root = tmp_path / "not-created"
    result = run_process(
        [str(ROOT / "fplinux"), "inspect", "inputs", "--site"],
        name="inspect unavailable site inputs",
        cwd=ROOT,
        env=os.environ | {"FPLINUX_DATA_DIR": str(root)},
        timeout=30,
    )

    assert result.returncode == 1
    assert "missing: source:site-node:" in result.stdout
    assert "site tooling: unavailable" in result.stdout
    assert "prepare inputs with: ./fplinux setup --site" in result.stdout
    assert not root.exists()


@dataclass(frozen=True)
class InspectCli:
    """Own the checkout and artifacts read by the public inspector."""

    root: Path

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Bound the public subprocess and retain its observable output."""
        return run_process(
            [str(self.root / "fplinux"), *arguments],
            name="public artifact inspection",
            cwd=self.root,
            timeout=10,
        )

    def make_bundle(
        self,
        generation: str,
        profile: str | None = None,
        *,
        build_type: str = "release",
        target: str = "example",
    ) -> Path:
        """Publish a normal fixture generation with independently described payload bytes."""
        output = self.root / ".data/out"
        output.mkdir(parents=True, exist_ok=True)
        staging = create_bundle_staging(output, target, profile, build_type=build_type)
        payload = staging / "payload.bin"
        payload.write_bytes(b"abc")
        manifest: dict[str, object] = {
            "target": target,
            "profile": profile,
            "build_type": build_type,
            "generation": generation,
            "files": {"payload.bin": file_record(payload)},
            "workspace_digest": "a" * 64,
            "release_id": "aaaaaaaaaaaa",
            "source_commit": "a" * 40,
            "source_snapshot": "b" * 64,
            "environment_recipe": "b" * 64,
            "environment_content": "c" * 64,
            "apk_signing_key": "d" * 64,
            "device_identity": "e" * 64,
            "linux_recipe": "f" * 64,
            "rootfs_receipt": {},
            "kernel_artifacts": {},
            "kbuild_receipt": {},
            "boot_artifacts": {"required": []},
        }
        (staging / "build-manifest.json").write_text(json.dumps(manifest))
        return publish_bundle_generation(
            output, target, staging, generation, profile, build_type=build_type
        )

    def make_archive(self, *, damaged: bool = False, extra: bool = False) -> Path:
        """Write an independently checksummed ZIP without calling the production packager."""
        manifest = {
            "target": "example",
            "release_id": "aaaaaaaaaaaa",
            "source_commit": "a" * 40,
            "source_snapshot": "b" * 64,
            "profile": None,
            "build_type": "release",
            "files": {"payload.bin": {"mode": 0o644, "size": 3, "sha256": ABC_SHA256}},
        }
        canonical = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode() + b"\n"
        manifest["generation"] = hashlib.sha256(canonical).hexdigest()
        contents = {"build-manifest.json": json.dumps(manifest).encode(), "payload.bin": b"abc"}
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
                item = zipfile.ZipInfo("bundle/" + name)
                item.external_attr = 0o100644 << 16
                archive.writestr(item, data)
            archive.writestr("bundle/SHA256SUMS", checksums)
        return path


@pytest.fixture
def inspect_cli(tmp_path: Path) -> InspectCli:
    """Keep every inspected artifact and process inside a disposable checkout."""
    prepare_cli_checkout(tmp_path)
    return InspectCli(tmp_path)


def test_bundle_comparison_reports_stale_source_and_unavailable_environment(
    tmp_path: Path,
) -> None:
    cache = tmp_path / ".data"
    inspector = InspectCli(tmp_path)
    bundle = inspector.make_bundle("1" * 64, target="nokia-ta1618")
    publish_current_bundle(cache / "out", "nokia-ta1618", bundle)

    result = run_process(
        [
            str(ROOT / "fplinux"),
            "inspect",
            "bundle",
            "nokia-ta1618",
            "--current-source",
            "--without-device-data",
        ],
        name="compare saved bundle with current source",
        cwd=ROOT,
        env=os.environ | {"FPLINUX_DATA_DIR": str(cache)},
        timeout=30,
    )

    assert result.returncode == 0, result.stderr
    assert "build-manifest checksums: OK" in result.stdout
    assert "source: stale" in result.stdout
    assert "workspace: stale" in result.stdout
    assert "environment: unavailable" in result.stdout
    assert "signing: unavailable" in result.stdout
    assert "bundle freshness: stale" in result.stdout

    card_bundle = inspector.make_bundle("2" * 64, "microsd-uboot", target="nokia-ta1618")
    publish_current_bundle(cache / "out", "nokia-ta1618", card_bundle, "microsd-uboot")
    phone = tmp_path / "selected-phone"
    phone.mkdir()
    (phone / "phone").write_text("not a prepared directory")
    selected = run_process(
        [
            str(ROOT / "fplinux"),
            "inspect",
            "bundle",
            "nokia-ta1618",
            "--profile",
            "microsd-uboot",
            "--current-source",
            "--phone-data",
            str(phone),
        ],
        name="compare using selected phone data",
        cwd=ROOT,
        env=os.environ
        | {"FPLINUX_DATA_DIR": str(cache), "XDG_DATA_HOME": str(tmp_path / "other")},
        timeout=30,
    )
    assert selected.returncode == 1
    assert str(phone / "phone") in selected.stderr
    assert "declared input directory is missing or invalid" in selected.stderr

    rejected = run_process(
        [str(ROOT / "fplinux"), "inspect", "bundle", "nokia-ta1618", "--phone-data", str(phone)],
        name="reject unused phone data selector",
        cwd=ROOT,
        env=os.environ | {"FPLINUX_DATA_DIR": str(cache)},
        timeout=30,
    )
    assert rejected.returncode == 1
    assert "require --current-source" in rejected.stderr


class InspectCliTests:
    """Use the actual archive libraries and resolver; no Kern or phone is involved."""

    def test_build_type_selection_is_explicit_and_never_falls_back(
        self, inspect_cli: InspectCli
    ) -> None:
        """The public inspector reports only the requested type's current generation."""
        output = inspect_cli.root / ".data/out"
        release = inspect_cli.make_bundle("1" * 64)
        publish_current_bundle(output, "example", release)
        missing = inspect_cli.run_cli("inspect", "bundle", "example", "--build-type", "debug")
        assert (missing.returncode) != (0)
        assert ("build example --build-type debug") in (missing.stderr)
        debug = inspect_cli.make_bundle("2" * 64, build_type="debug")
        publish_current_bundle(output, "example", debug, build_type="debug")

        for build_type, generation in (
            ("release", "1" * 64),
            ("debug", "2" * 64),
            ("release", "1" * 64),
        ):
            result = inspect_cli.run_cli(
                "inspect", "bundle", "example", "--build-type", build_type
            )
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
        self, inspect_cli: InspectCli, arguments: tuple[str, ...], generation: str, profile: str
    ) -> None:
        """A newer unselected generation must not replace the published selection."""
        output = inspect_cli.root / ".data/out"
        selected = inspect_cli.make_bundle("1" * 64)
        publish_current_bundle(output, "example", selected)
        inspect_cli.make_bundle("2" * 64)
        card = inspect_cli.make_bundle("3" * 64, "microsd-uboot")
        publish_current_bundle(output, "example", card, "microsd-uboot")
        result = inspect_cli.run_cli("inspect", "bundle", "example", *arguments)
        assert (result.returncode) == (0), result.stderr
        assert (f"generation: {generation}\n") in (result.stdout)
        assert (f"profile: {profile}\n") in (result.stdout)
        assert (f"3 {ABC_SHA256} payload.bin\n") in (result.stdout)
        assert ("build-manifest checksums: OK") in (result.stdout)
        assert (str(inspect_cli.root)) not in (result.stdout)

    def test_archive_reports_identity_and_checks_every_listed_file_without_extraction(
        self, inspect_cli: InspectCli
    ) -> None:
        """Runtime archives report verified bytes without creating an extracted tree."""
        archive = inspect_cli.make_archive()
        original = archive.read_bytes()
        result = inspect_cli.run_cli("inspect", "archive", archive.name)
        assert (result.returncode) == (0), result.stderr
        assert ("kind: runtime\n") in (result.stdout)
        assert "release_id: aaaaaaaaaaaa\n" in result.stdout
        assert "source_commit: " + "a" * 40 + "\n" in result.stdout
        assert "source_snapshot: " + "b" * 64 + "\n" in result.stdout
        assert ("target: example\n") in (result.stdout)
        assert (f"3 {ABC_SHA256} payload.bin\n") in (result.stdout)
        assert ("SHA256SUMS: OK") in (result.stdout)
        assert not ((inspect_cli.root / "bundle").exists())
        assert not ((inspect_cli.root / ".data").exists())
        assert (archive.read_bytes()) == (original)

    @pytest.mark.parametrize(
        ("damaged", "extra", "error"),
        [
            pytest.param(True, False, "checksum mismatch", id="changed-payload"),
            pytest.param(False, True, "inventory", id="unlisted-file"),
        ],
    )
    def test_archive_rejects_changed_bytes_and_unlisted_files(
        self, inspect_cli: InspectCli, *, damaged: bool, extra: bool, error: str
    ) -> None:
        """Reject modified payloads and incomplete checksum inventories."""
        archive = inspect_cli.make_archive(damaged=damaged, extra=extra)
        result = inspect_cli.run_cli("inspect", "archive", archive.name)
        assert (result.returncode) == (1), result.stderr
        assert (error) in (result.stderr)
        assert ("SHA256SUMS: OK") not in (result.stdout)
        assert ("Traceback") not in (result.stderr)

    def test_apk_reads_all_gzip_sections_metadata_and_symlinks_without_installing(
        self, inspect_cli: InspectCli
    ) -> None:
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
        path = inspect_cli.root / "example.apk"
        path.write_bytes(b"".join(parts))
        result = inspect_cli.run_cli("inspect", "apk", path.name)
        assert (result.returncode) == (0), result.stderr
        for expected in (
            "pkgname = example",
            "depend = alpha\ndepend = beta",
            ".post-install",
            "file 3 usr/bin/example",
            "symlink 0 usr/bin/link -> example",
        ):
            assert (expected) in (result.stdout)
        assert not ((inspect_cli.root / "usr").exists())
        assert not ((inspect_cli.root / ".data").exists())

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
        self, inspect_cli: InspectCli, kind: str, name: str
    ) -> None:
        """Missing and unreadable archive formats produce ordinary CLI errors."""
        (inspect_cli.root / "invalid").write_bytes(b"not an archive")
        result = inspect_cli.run_cli("inspect", kind, name)
        assert (result.returncode) == (1), result.stderr
        assert ("Traceback") not in (result.stderr)
        assert not ((inspect_cli.root / ".data").exists())
