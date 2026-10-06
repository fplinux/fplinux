# SPDX-License-Identifier: GPL-2.0-only
"""Measure synthetic artifacts and compare saved reports through the public CLI."""

from __future__ import annotations

import hashlib
import json
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.artifacts.bundles import (
    create_bundle_staging,
    publish_bundle_generation,
    publish_current_bundle,
)

from tests.cli_support import prepare_cli_checkout
from tests.fixtures.artifact_footprint import (
    FootprintFixture,
    kernel_with_initramfs,
    squashfs_header,
)
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from pathlib import Path


class FootprintCliTests:
    """Use real archives and the public resolver without a build runtime or phone."""

    @pytest.fixture(autouse=True)
    def _prepare_inputs(self, tmp_path: Path) -> None:
        """Keep commands and artifacts in an isolated checkout."""
        self.root = tmp_path
        prepare_cli_checkout(self.root)

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run the actual public entry point with a bounded lifetime."""
        return run_process(
            [str(self.root / "fplinux"), *arguments],
            name="public footprint inspection",
            cwd=self.root,
            timeout=10,
        )

    def publish_bundle(self, *, profile: str | None = None, build_type: str = "release") -> Path:
        """Publish independent archive fixtures with a complete current-bundle manifest."""
        output = self.root / ".cache/out"
        output.mkdir(parents=True, exist_ok=True)
        staging = create_bundle_staging(output, "example", profile, build_type=build_type)
        bundle = FootprintFixture(staging).bundle(external=profile is not None)
        manifest = json.loads(bundle.manifest_bytes)
        manifest.update(
            target="example",
            profile=profile,
            build_type=build_type,
            workspace_digest="a" * 64,
            container_image_recipe="b" * 64,
            container_image_content="c" * 64,
            apk_signing_key="d" * 64,
            device_identity="e" * 64,
            linux_recipe="f" * 64,
            rootfs_receipt={},
            kbuild_receipt={},
        )
        (staging / "build-manifest.json").write_text(json.dumps(manifest))
        published = publish_bundle_generation(
            output, "example", staging, bundle.generation, profile, build_type=build_type
        )
        publish_current_bundle(output, "example", published, profile, build_type=build_type)
        return published

    @pytest.mark.parametrize(
        ("profile", "build_type"),
        [
            pytest.param(None, "release", id="default-release"),
            pytest.param(None, "debug", id="default-debug"),
            pytest.param("microsd-uboot", "release", id="microsd-release"),
        ],
    )
    def test_footprint_selects_exact_profile_and_build_type_in_json_and_text(
        self, profile: str | None, build_type: str
    ) -> None:
        """Readable and machine reports retain the selected identity and measured bytes."""
        self.publish_bundle()
        self.publish_bundle(build_type="debug")
        self.publish_bundle(profile="microsd-uboot")
        arguments = ["inspect", "footprint", "example", "--build-type", build_type]
        if profile is not None:
            arguments.extend(["--profile", profile])
        result = self.run_cli(*arguments, "--json")
        assert (result.returncode) == (0), result.stderr
        report = json.loads(result.stdout)
        assert (report["identity"]["target"]) == ("example")
        assert (report["identity"]["profile"]) == (profile)
        assert (report["identity"]["build_type"]) == (build_type)
        assert (report["layers"]["kernel_zimage_bytes"]) == (12)
        if profile is None:
            assert (report["rootfs"]["source"]) == ("ram-squashfs-composition")
            assert (report["layers"]["ram_root"]["compression"]) == ("xz")
            assert (report["layers"]["ram_root"]["block_bytes"]) == (65536)
            assert (report["layers"]["ram_root"]["bytes"]) == (120)
            assert (report["layers"]["ram_root"]["sha256"]) == (
                hashlib.sha256(squashfs_header("xz")).hexdigest()
            )
            assert (report["layers"]["embedded_initramfs"]["compression"]) == ("gzip")
        else:
            assert (report["layers"]["ram_root"]) is None
            assert (report["layers"]["embedded_initramfs"]) is None
        assert (report["rootfs"]["packages"]["shared"]["regular_payload_bytes"]) == (7)
        assert (report["rootfs"]["files"]["/bin/one-link"]["accounted_bytes"]) == (0)
        text = self.run_cli(*arguments)
        assert (text.returncode) == (0), text.stderr
        assert ("Kernel zImage: 12 B") in (text.stdout)
        assert ("7 B  shared 1-r0") in (text.stdout)
        assert ("Nested layers overlap") in (text.stdout)
        assert (str(self.root)) not in (text.stdout)

    def test_missing_build_type_fails_without_measurements(self) -> None:
        """A release bundle cannot satisfy a request for a debug report."""
        self.publish_bundle()
        missing = self.run_cli(
            "inspect", "footprint", "example", "--build-type", "debug", "--json"
        )
        assert (missing.returncode) == (1), missing.stderr
        assert ("build example --build-type debug") in (missing.stderr)
        assert (missing.stdout) == ("")

    @pytest.mark.parametrize(
        "payload",
        [
            pytest.param("debug/rootfs.cpio", id="composition"),
            pytest.param("debug/initramfs.cpio", id="boot-archive"),
        ],
    )
    def test_missing_payload_fails_without_partial_report_or_traceback(self, payload: str) -> None:
        """A selected RAM bundle must retain both composition and boot archives."""
        published = self.publish_bundle()
        path = published / payload
        original = path.read_bytes()
        path.unlink()
        try:
            result = self.run_cli("inspect", "footprint", "example", "--json")
            assert (result.returncode) == (1), result.stderr
            assert (result.stdout) == ("")
            assert (payload) in (result.stderr)
            assert ("Traceback") not in (result.stderr)
        finally:
            path.write_bytes(original)

    def test_kernel_embedding_composition_instead_of_boot_archive_is_rejected(self) -> None:
        """Individually valid artifacts cannot report the wrong embedded boot content."""
        output = self.root / ".cache/out"
        output.mkdir(parents=True)
        staging = create_bundle_staging(output, "example")
        fixture = FootprintFixture(staging)
        bundle = fixture.bundle()
        elf = kernel_with_initramfs(fixture.composition, "gzip")
        (staging / "debug/vmlinux").write_bytes(elf)
        manifest = json.loads(bundle.manifest_bytes)
        manifest.update(
            target="example",
            profile=None,
            build_type="release",
            workspace_digest="a" * 64,
            container_image_recipe="b" * 64,
            container_image_content="c" * 64,
            apk_signing_key="d" * 64,
            device_identity="e" * 64,
            linux_recipe="f" * 64,
            rootfs_receipt={},
            kbuild_receipt={},
        )
        manifest["files"]["debug/vmlinux"] = {
            "size": len(elf),
            "sha256": hashlib.sha256(elf).hexdigest(),
            "mode": 0o644,
        }
        (staging / "build-manifest.json").write_text(json.dumps(manifest))
        published = publish_bundle_generation(output, "example", staging, bundle.generation)
        publish_current_bundle(output, "example", published)
        result = self.run_cli("inspect", "footprint", "example", "--json")
        assert (result.returncode) == (1), result.stderr
        assert (result.stdout) == ("")
        assert ("embedded initramfs differs") in (result.stderr)
        assert ("Traceback") not in (result.stderr)

    def write_reports(self) -> None:
        """Save small independent measurements with a known kernel and package delta."""
        before: dict[str, Any] = {
            "identity": {"target": "example", "generation": "before"},
            "layers": {
                "kernel_zimage_bytes": 1024,
                "boot_artifact_bytes": {"image/ramboot.bin": 2048},
                "embedded_initramfs": {"compression": "gzip"},
            },
            "rootfs": {
                "content_sha256": "a" * 64,
                "cpio_bytes": 512,
                "regular_payload_bytes": 100,
                "symlink_payload_bytes": 0,
                "packages": {"example": {"version": "1-r0"}},
                "files": {"/bin/example": {"size": 100}},
            },
            "optional_apks": {},
        }
        (self.root / "before.json").write_text(json.dumps(before))
        before["layers"]["kernel_zimage_bytes"] = 2048
        before["rootfs"]["packages"]["example"]["version"] = "2-r0"
        (self.root / "after.json").write_text(json.dumps(before))

    def test_diff_reports_saved_deltas_without_creating_cache(self) -> None:
        """Comparing saved measurements needs neither bundles nor build state."""
        self.write_reports()
        result = self.run_cli("inspect", "footprint-diff", "before.json", "after.json", "--json")
        assert (result.returncode) == (0), result.stderr
        report = json.loads(result.stdout)
        assert (report["kernel_zimage_byte_delta"]) == (1024)
        assert (report["packages"]["changed"]["example"]["after"]["version"]) == ("2-r0")
        assert report["same_rootfs_content"]
        result = self.run_cli("inspect", "footprint-diff", "before.json", "after.json")
        assert (result.returncode) == (0), result.stderr
        assert ("Kernel zImage delta: 1,024 B (1.0 KiB)") in (result.stdout)
        assert ("changed: example") in (result.stdout)
        assert not ((self.root / ".cache").exists())

    @pytest.mark.parametrize(
        "name",
        ["missing.json", "invalid.json", "array.json", "empty.json"],
        ids=["missing", "invalid", "array", "empty"],
    )
    def test_diff_rejects_missing_invalid_and_unrelated_json_without_traceback(
        self, name: str
    ) -> None:
        """Ordinary input mistakes yield CLI errors rather than partial comparisons."""
        self.write_reports()
        for fixture_name, content in (
            ("invalid.json", "broken"),
            ("array.json", "[]"),
            ("empty.json", "{}"),
        ):
            (self.root / fixture_name).write_text(content)
        result = self.run_cli("inspect", "footprint-diff", name, "after.json", "--json")
        assert (result.returncode) == (1), result.stderr
        assert (result.stdout) == ("")
        assert ("Traceback") not in (result.stderr)
        assert not ((self.root / ".cache").exists())

    @pytest.mark.parametrize(
        "arguments",
        [
            pytest.param(("run",), id="run"),
            pytest.param(("device-data", "prepare"), id="device-data-prepare"),
        ],
    )
    def test_events_help_and_saved_dump_rejection_are_available_without_phone(
        self, arguments: tuple[str, ...]
    ) -> None:
        """Both loader commands expose event output and offline extraction rejects it."""
        result = self.run_cli(*arguments, "--help")
        assert (result.returncode) == (0), result.stderr
        assert ("--events PATH") in (result.stdout)
        result = self.run_cli(
            "device-data",
            "prepare",
            "example",
            "--from-dump",
            "saved.bin",
            "--events",
            "events.jsonl",
        )
        assert (result.returncode) == (1), result.stderr
        assert ("cannot be used with --from-dump") in (result.stderr)
        assert not ((self.root / "events.jsonl").exists())
