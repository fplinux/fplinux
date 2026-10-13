# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for setup inputs staged without network access."""

from __future__ import annotations

import base64
import hashlib
import io
import json
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.cli.dependencies import restore_dependencies
from fplinux_cli.dependencies.inputs import environment_inputs
from fplinux_cli.dependencies.snapshots import preserve_inputs, publish_snapshot
from fplinux_cli.environment import downloads, images, kern
from fplinux_cli.environment import setup as env_setup
from fplinux_cli.environment.install_wheels import install_wheels

from tests.small.dependencies.snapshot_fixtures import selected_checkout

_FIXTURES = Path(__file__).parents[2] / "fixtures/environment/offline"


def _environment_checkout(root: Path) -> None:
    """Declare small source, npm and APK inputs without installing their contents."""
    source_bytes = b"tool source\n"
    npm_bytes = b"npm package\n"
    apk_bytes = b"signed package fixture\n"
    (root / ".kernignore").write_bytes((_FIXTURES / ".kernignore.fixture").read_bytes())
    source_digest = hashlib.sha256(source_bytes).hexdigest()
    (root / "Containerfile").write_text(
        (_FIXTURES / "Containerfile.in").read_text().format(sha256=source_digest)
    )
    image_content = root / "scripts/fplinux_cli/environment/image_content.py"
    image_content.parent.mkdir(parents=True)
    image_content.write_text('"""Synthetic image-context input; no image build is exercised."""\n')
    installer = root / "scripts/fplinux_cli/environment/install_wheels.py"
    installer.write_text('"""Synthetic installer-context input; no image build is exercised."""\n')
    (root / "package.json").write_text('{"name":"fixture","version":"1.0.0"}\n')
    npm_integrity = base64.b64encode(hashlib.sha512(npm_bytes).digest()).decode()
    (root / "package-lock.json").write_text(
        json.dumps(
            {
                "packages": {
                    "": {"name": "fixture", "version": "1.0.0"},
                    "node_modules/example": {
                        "version": "1.0.0",
                        "resolved": "https://example.invalid/example-1.0.0.tgz",
                        "integrity": f"sha512-{npm_integrity}",
                    },
                }
            }
        )
    )
    patch = root / "alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch"
    patch.parent.mkdir(parents=True)
    patch.write_text("terminal patch fixture\n")
    (root / "container.lock.toml").write_bytes((_FIXTURES / "container.lock.toml").read_bytes())
    (root / "environment.lock.toml").write_text(
        (_FIXTURES / "environment.lock.toml.in.fixture")
        .read_text()
        .format(sha256=hashlib.sha256(apk_bytes).hexdigest(), size=len(apk_bytes))
    )
    npm_key = hashlib.sha256(b"https://example.invalid/example-1.0.0.tgz").hexdigest()[:16]
    files = {
        "downloads/environment/tool/tool.tar.gz": source_bytes,
        f"downloads/npm/{npm_key}-example-1.0.0.tgz": npm_bytes,
        "downloads/environment/apk/main/x86_64/tool-1.0-r0.apk": apk_bytes,
        "private/phone-state": b"private fixture\n",
    }
    for relative, contents in files.items():
        path = root / ".cache" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)


class OfflineRuntimeInputTests:
    """Install the pinned runtime from a saved archive and report cache misses."""

    def test_saved_archive_installs_the_exact_binary_without_downloading(self) -> None:
        """Verified cached archive bytes suffice when no project runtime exists."""
        executable = b"static runtime fixture\n"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / ".cache/downloads/kern/kern.tar.gz"
            archive.parent.mkdir(parents=True)
            with tarfile.open(archive, "w:gz") as bundle:
                member = tarfile.TarInfo("kern")
                member.size = len(executable)
                bundle.addfile(member, io.BytesIO(executable))
            lock = {
                "kern": {
                    "archive_url": "https://example.invalid/kern.tar.gz",
                    "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                    "binary_sha256": hashlib.sha256(executable).hexdigest(),
                }
            }
            with (
                mock.patch.object(kern, "ROOT", root),
                mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
            ):
                installed = Path(kern.install_kern(lock, offline=True))
            assert (installed.read_bytes()) == (b"static runtime fixture\n")
            assert (installed.stat().st_mode & 0o777) == (0o755)

    @pytest.mark.parametrize(
        "cached_bytes", [None, b"previous source bytes\n"], ids=["missing", "cached-mismatch"]
    )
    def test_offline_locked_file_lookup_rejects_missing_archive_without_network(
        self, cached_bytes: bytes | None
    ) -> None:
        """An unavailable checksum match reports its URL without opening the network."""
        with (
            tempfile.TemporaryDirectory() as temporary,
        ):
            destination = Path(temporary) / "kern.tar.gz"
            if cached_bytes is not None:
                destination.write_bytes(cached_bytes)
            with (
                mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
                pytest.raises(
                    SystemExit, match=r"offline locked input.*https://example.invalid/kern.tar.gz"
                ),
            ):
                downloads.download_locked_file(
                    "https://example.invalid/kern.tar.gz",
                    hashlib.sha256(b"new source bytes\n").hexdigest(),
                    destination,
                    offline=True,
                )
            if cached_bytes is not None:
                assert (destination.read_bytes()) == (b"previous source bytes\n")
            else:
                assert not (destination.exists())


class OfflineEnvironmentContextTests:
    """Stage exact declared bytes while excluding unrelated project cache state."""

    def test_declared_schema_wheel_survives_snapshot_restore_and_staging(self) -> None:
        """A fresh cache delivers the original wheel to the Python extraction boundary."""
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            root = parent / "checkout"
            root.mkdir()
            _environment_checkout(root)
            metadata = (_FIXTURES / "METADATA").read_bytes()
            stream = io.BytesIO()
            with zipfile.ZipFile(stream, "w") as wheel:
                wheel.writestr(zipfile.ZipInfo("dtschema-2026.9.dist-info/METADATA"), metadata)
            contents = stream.getvalue()
            lock = root / "environment.lock.toml"
            with lock.open("a") as output:
                output.write(
                    (_FIXTURES / "dtschema-wheel.toml.in.fixture")
                    .read_text()
                    .format(sha256=hashlib.sha256(contents).hexdigest(), size=len(contents))
                )
            wheel_path = (
                root / ".cache/downloads/environment/dtschema/dtschema-2026.9-py3-none-any.whl"
            )
            wheel_path.parent.mkdir(parents=True)
            wheel_path.write_bytes(contents)
            inputs = [
                item
                for item in environment_inputs(root)
                if item.purpose not in {"container-base", "kern-runtime"}
            ]
            snapshot = parent / "saved-inputs"
            manifest = preserve_inputs(inputs, {}, snapshot, cache=root / ".cache", offline=True)
            publish_snapshot(snapshot, manifest)
            restored_root = parent / "restored-checkout"
            shutil.copytree(root, restored_root, ignore=shutil.ignore_patterns(".cache"))
            context = parent / "context"
            context.mkdir()
            with (
                mock.patch.object(env_setup, "ROOT", restored_root),
                selected_checkout(restored_root, inputs, {}),
                mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
            ):
                restore_dependencies(snapshot, inputs_only=True)
                env_setup._stage_container_context(context, offline=True)  # noqa: SLF001
            staged = context / "inputs/sources/dtschema/dtschema-2026.9-py3-none-any.whl"
            assert (staged.read_bytes()) == (contents)
            with zipfile.ZipFile(staged) as wheel:
                assert (wheel.read("dtschema-2026.9.dist-info/METADATA")) == (
                    b"Metadata-Version: 2.1\nName: dtschema\nVersion: 2026.9\n"
                )
            installed = parent / "python"
            with mock.patch("subprocess.run", side_effect=AssertionError("network")):
                install_wheels(
                    context / "environment.lock.toml", context / "inputs", installed, offline=True
                )
            assert (installed / "dtschema-2026.9.dist-info/METADATA").read_bytes() == (
                b"Metadata-Version: 2.1\nName: dtschema\nVersion: 2026.9\n"
            )

    def test_context_contains_declared_inputs_and_excludes_private_cache(self) -> None:
        """Source, npm and package consumers receive original saved bytes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            context = Path(temporary) / "context"
            root.mkdir()
            context.mkdir()
            _environment_checkout(root)
            installer = root / "scripts/fplinux_cli/environment/install_wheels.py"
            installer.chmod(0o755)
            with mock.patch.object(env_setup, "ROOT", root):
                env_setup._stage_container_context(context, offline=True)  # noqa: SLF001

            assert (context / "environment.lock.toml").read_bytes() == (
                root / "environment.lock.toml"
            ).read_bytes()
            assert (
                context / "scripts/fplinux_cli/environment/install_wheels.py"
            ).read_bytes() == installer.read_bytes()
            assert (
                context / "scripts/fplinux_cli/environment/install_wheels.py"
            ).stat().st_mode & 0o777 == 0o755

            assert ((context / "inputs/sources/tool/tool.tar.gz").read_bytes()) == (
                b"tool source\n"
            )
            npm_archives = list((context / "inputs/npm").glob("*.tgz"))
            assert (len(npm_archives)) == (1)
            assert (npm_archives[0].read_bytes()) == (b"npm package\n")
            assert ((context / "inputs/apk/main/x86_64/tool-1.0-r0.apk").read_bytes()) == (
                b"signed package fixture\n"
            )
            assert not ((context / ".cache").exists())
            assert not ((context / "inputs/private").exists())

    def test_missing_declared_environment_input_names_the_exact_source(self) -> None:
        """Offline staging stops with the requested URL instead of silently omitting a source."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            context = Path(temporary) / "context"
            root.mkdir()
            context.mkdir()
            _environment_checkout(root)
            (root / ".cache/downloads/environment/tool/tool.tar.gz").unlink()
            with (
                mock.patch.object(env_setup, "ROOT", root),
                pytest.raises(
                    SystemExit, match=r"offline locked input.*https://example.invalid/tool"
                ),
            ):
                env_setup._stage_container_context(context, offline=True)  # noqa: SLF001

    def test_updated_source_checksum_requires_the_new_saved_bytes(self) -> None:
        """A declaration change treats the older saved source as an offline cache miss."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            context = Path(temporary) / "context"
            root.mkdir()
            context.mkdir()
            _environment_checkout(root)
            container = root / "Containerfile"
            container.write_text(
                container.read_text().replace(
                    hashlib.sha256(b"tool source\n").hexdigest(),
                    hashlib.sha256(b"updated tool source\n").hexdigest(),
                )
            )
            with (
                mock.patch.object(env_setup, "ROOT", root),
                pytest.raises(SystemExit, match="offline locked input is missing or mismatched"),
            ):
                env_setup._stage_container_context(context, offline=True)  # noqa: SLF001
            assert not ((context / "inputs/sources/tool/tool.tar.gz").exists())

    def test_online_context_reuses_exact_saved_inputs_without_network(self) -> None:
        """Online setup stages the same locked closure instead of resolving packages again."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            context = Path(temporary) / "context"
            root.mkdir()
            context.mkdir()
            _environment_checkout(root)
            with (
                mock.patch.object(env_setup, "ROOT", root),
                mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
            ):
                env_setup._stage_container_context(context, offline=False)  # noqa: SLF001
            assert ((context / "inputs/sources/tool/tool.tar.gz").read_bytes()) == (
                b"tool source\n"
            )
            assert ((context / "package.json").read_bytes()) == (
                (root / "package.json").read_bytes()
            )

    def test_online_context_downloads_a_missing_declared_source(self) -> None:
        """A host download supplies verified original bytes to the otherwise local recipe."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            context = Path(temporary) / "context"
            root.mkdir()
            context.mkdir()
            _environment_checkout(root)
            cached_source = root / ".cache/downloads/environment/tool/tool.tar.gz"
            cached_source.unlink()
            with (
                mock.patch.object(env_setup, "ROOT", root),
                mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"tool source\n")),
            ):
                env_setup._stage_container_context(context, offline=False)  # noqa: SLF001
            assert (cached_source.read_bytes()) == (b"tool source\n")
            assert ((context / "inputs/sources/tool/tool.tar.gz").read_bytes()) == (
                b"tool source\n"
            )

    def test_download_checksum_failure_preserves_previous_cached_source(self) -> None:
        """An updated source declaration cannot publish an unverified host response."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            context = Path(temporary) / "context"
            root.mkdir()
            context.mkdir()
            _environment_checkout(root)
            container = root / "Containerfile"
            container.write_text(
                container.read_text().replace(
                    hashlib.sha256(b"tool source\n").hexdigest(),
                    hashlib.sha256(b"updated tool source\n").hexdigest(),
                )
            )
            with (
                mock.patch.object(env_setup, "ROOT", root),
                mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"wrong response\n")),
                pytest.raises(SystemExit, match="locked download SHA256 mismatch"),
            ):
                env_setup._stage_container_context(context, offline=False)  # noqa: SLF001
            assert ((root / ".cache/downloads/environment/tool/tool.tar.gz").read_bytes()) == (
                b"tool source\n"
            )
            assert not ((context / "inputs/sources/tool/tool.tar.gz").exists())

    def test_environment_lock_changes_the_image_recipe(self) -> None:
        """Changing locked package bytes invalidates an existing image identity."""
        lock = {
            "kern": {"binary_sha256": "c" * 64},
            "oci": {
                "base_repository": "localhost/fplinux-alpine-base",
                "base_release": "3.24.2",
                "base_rootfs_sha256": "d" * 64,
            },
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _environment_checkout(root)
            with mock.patch.object(common, "ROOT", root):
                original = images.container_image_recipe_digest(lock)
                (root / "unrelated.txt").write_text("unrelated source\n")
                assert (images.container_image_recipe_digest(lock)) == (original)
                environment_lock = root / "environment.lock.toml"
                environment_lock.write_text(
                    environment_lock.read_text().replace("tool-1.0", "tool-1.1")
                )
                assert (images.container_image_recipe_digest(lock)) != (original)
