# SPDX-License-Identifier: GPL-2.0-only
"""Preserve and restore the declared external build inputs by their exact bytes."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import tarfile
import tempfile
from dataclasses import asdict
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import canonical_json_bytes, fail, replace_file_atomically, sha256_file
from fplinux_cli.dependency_inputs import (
    DependencyInput,
    dependency_context,
    dependency_inputs,
)
from fplinux_cli.dependency_site_inputs import (
    remember_site_inputs,
    resolve_site_inputs,
    site_dependency_context,
)
from fplinux_cli.environment import images
from fplinux_cli.environment import kern as kern_env
from fplinux_cli.image_content import validate_image_metadata
from fplinux_cli.image_state import ImageState
from fplinux_cli.output import RunReporter

if TYPE_CHECKING:
    from collections.abc import Sequence


_ENVIRONMENT_TIMEOUT = 2 * 60 * 60


def _directory(path: Path) -> None:
    """Prepare an owned directory while rejecting links in its existing parents."""
    for component in (path, *path.parents):
        if component.is_symlink() or (component.exists() and not component.is_dir()):
            fail(f"dependency directory is invalid: {component}")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)


def _snapshot_directory(directory: Path, cache: Path) -> Path:
    path = directory.absolute()
    if path.resolve().is_relative_to(cache.resolve()):
        fail("dependency snapshots must be stored outside the working .cache directory")
    _directory(path)
    return path


def _object_path(directory: Path, digest: str) -> Path:
    if len(digest) != 64 or any(character not in "0123456789abcdef" for character in digest):
        fail("dependency object SHA-256 is invalid")
    return directory / "objects/sha256" / digest


def _copy_verified(source: Path, destination: Path, digest: str, *, mode: int = 0o600) -> None:
    """Stream a large object into its final slot only after validating copied bytes."""
    _directory(destination.parent)
    if destination.is_symlink() or (destination.exists() and not destination.is_file()):
        fail(f"dependency destination is invalid: {destination}")
    if destination.is_file() and sha256_file(destination) == digest:
        return
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent, prefix=".input-", delete=False
        ) as output:
            temporary = Path(output.name)
            with source.open("rb") as stream:
                shutil.copyfileobj(stream, output)
            output.flush()
            os.fsync(output.fileno())
        if sha256_file(temporary) != digest:
            fail(f"dependency changed while being copied: {source}")
        temporary.chmod(mode)
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _checksum(input_file: DependencyInput, path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    if input_file.size is not None and path.stat().st_size != input_file.size:
        return False
    expected = input_file.checksum or input_file.sha256
    if expected is None or input_file.algorithm not in {"sha256", "sha512"}:
        fail(f"dependency has no supported exact checksum: {input_file.key}")
    with path.open("rb") as stream:
        actual = hashlib.file_digest(stream, input_file.algorithm).hexdigest()
    return actual == expected


def _source_index(
    sources: Sequence[Path], inputs: Sequence[DependencyInput]
) -> dict[tuple[str, str], Path]:
    """Index explicitly selected originals by their declared native checksums."""
    required = {(item.algorithm, item.checksum or item.sha256) for item in inputs}
    algorithms = sorted({algorithm for algorithm, _ in required})
    matches: dict[tuple[str, str], Path] = {}
    for source in sources:
        candidates = sorted(source.rglob("*")) if source.is_dir() else [source]
        for path in candidates:
            if path.is_symlink() or not path.is_file():
                continue
            for algorithm in algorithms:
                with path.open("rb") as stream:
                    digest = hashlib.file_digest(stream, algorithm).hexdigest()
                if (algorithm, digest) in required:
                    matches.setdefault((algorithm, digest), path)
    return matches


def _saved_input(
    input_file: DependencyInput, cache: Path, sources: dict[tuple[str, str], Path]
) -> Path | None:
    destination = cache / input_file.destination
    if _checksum(input_file, destination):
        return destination
    expected = input_file.checksum or input_file.sha256
    if expected is not None:
        candidate = sources.get((input_file.algorithm, expected))
        if candidate is not None and _checksum(input_file, candidate):
            return candidate
    return None


def _fetch_input(input_file: DependencyInput, cache: Path) -> Path:
    expected = input_file.checksum or input_file.sha256
    if expected is None:
        fail(f"dependency has no exact checksum: {input_file.key}")
    return kern_env._download_locked_file(  # noqa: SLF001 -- share the native checksum transport.
        input_file.url,
        expected,
        cache / input_file.destination,
        algorithm=input_file.algorithm,
        size=input_file.size,
    )


def preserve_inputs(  # noqa: PLR0913 -- closure, storage and acquisition policies are distinct inputs.
    inputs: Sequence[DependencyInput],
    context: dict[str, Any],
    directory: Path,
    *,
    cache: Path,
    offline: bool,
    sources: Sequence[Path] = (),
) -> dict[str, Any]:
    """Preserve explicitly declared files; an absent exact input stops publication."""
    directory = _snapshot_directory(directory, cache)
    for source_path in sources:
        if source_path.is_symlink() or not (source_path.is_dir() or source_path.is_file()):
            fail(f"dependency source path is missing or invalid: {source_path}")
        if source_path.resolve() == cache.resolve():
            fail("select an original input directory instead of the complete working cache")
    source_files = _source_index(sources, inputs)
    records: list[dict[str, Any]] = []
    missing: list[str] = []
    for input_file in sorted(inputs, key=lambda item: item.key):
        source = _saved_input(input_file, cache, source_files)
        if source is None:
            if offline:
                missing.append(f"{input_file.key}: {input_file.url}")
                continue
            try:
                source = _fetch_input(input_file, cache)
            except OSError as error:
                fail(f"dependency download failed: {input_file.key}: {input_file.url}: {error}")
        digest = sha256_file(source)
        _copy_verified(source, _object_path(directory, digest), digest)
        records.append(
            {"input": asdict(input_file), "sha256": digest, "bytes": source.stat().st_size}
        )
    if missing:
        fail("exact dependency inputs are unavailable:\n" + "\n".join(missing))
    identity = {"context": context, "inputs": records}
    return {
        "snapshot": common.sha256_bytes(canonical_json_bytes(identity)),
        **identity,
        "environment": None,
    }


def publish_snapshot(directory: Path, manifest: dict[str, Any]) -> None:
    """Publish a verified input set without replacing another saved snapshot."""
    destination = directory / "manifest.json"
    if destination.exists() or destination.is_symlink():
        previous = read_snapshot(directory)
        if previous["snapshot"] != manifest["snapshot"]:
            fail(
                "dependency directory already contains another snapshot; choose another directory"
            )
        # The first preserved exact image remains attached to this external input set.
        if previous["environment"] is not None:
            return
    replace_file_atomically(destination, canonical_json_bytes(manifest), 0o600)


def read_snapshot(directory: Path) -> dict[str, Any]:
    """Validate the fixed manifest and every referenced object before restoration."""
    path = directory / "manifest.json"
    if path.is_symlink() or not path.is_file():
        fail(f"dependency manifest is missing or invalid: {path}")
    try:
        value = json.loads(path.read_bytes())
    except (OSError, ValueError) as error:
        fail(f"dependency manifest cannot be read: {path}: {error}")
    if not isinstance(value, dict) or set(value) != {
        "snapshot",
        "context",
        "inputs",
        "environment",
    }:
        fail("dependency manifest fields are invalid")
    records = value["inputs"]
    if not isinstance(value["context"], dict) or not isinstance(records, list):
        fail("dependency manifest context or inputs are invalid")
    keys: set[str] = set()
    destinations: dict[str, str] = {}
    for record in records:
        if not isinstance(record, dict) or set(record) != {"input", "sha256", "bytes"}:
            fail("dependency manifest input record is invalid")
        try:
            input_file = DependencyInput(**record["input"])
        except (TypeError, ValueError) as error:
            fail(f"dependency declaration is invalid: {error}")
        common.relative_name(input_file.destination, field="dependency cache destination")
        if input_file.key in keys:
            fail(f"dependency manifest contains a duplicate input: {input_file.key}")
        keys.add(input_file.key)
        digest = record["sha256"]
        if not isinstance(digest, str) or type(record["bytes"]) is not int or record["bytes"] < 0:
            fail(f"dependency object metadata is invalid: {input_file.key}")
        object_path = _object_path(directory, digest)
        previous_digest = destinations.get(input_file.destination)
        if previous_digest is not None and previous_digest != digest:
            fail(f"dependency cache destination has conflicting bytes: {input_file.destination}")
        destinations[input_file.destination] = digest
        if not _checksum(input_file, object_path) or object_path.stat().st_size != record["bytes"]:
            fail(f"dependency object is missing or mismatched: {input_file.key}: {digest}")
        if sha256_file(object_path) != digest:
            fail(f"dependency object SHA-256 mismatch: {input_file.key}: {digest}")
    identity = {"context": value["context"], "inputs": records}
    if value["snapshot"] != common.sha256_bytes(canonical_json_bytes(identity)):
        fail("dependency snapshot identity does not match its manifest")
    environment = value["environment"]
    if environment is not None:
        if not isinstance(environment, dict) or set(environment) != {
            "sha256",
            "bytes",
            "reference",
            "state",
            "transport",
            "metadata",
        }:
            fail("dependency environment fields are invalid")
        digest = environment["sha256"]
        if not isinstance(digest, str) or type(environment["bytes"]) is not int:
            fail("dependency environment object metadata is invalid")
        object_path = _object_path(directory, digest)
        if (
            object_path.is_symlink()
            or not object_path.is_file()
            or object_path.stat().st_size != environment["bytes"]
            or sha256_file(object_path) != digest
        ):
            fail("dependency environment object is missing or mismatched")
        try:
            ImageState(**environment["state"])
        except (TypeError, ValueError) as error:
            fail(f"dependency environment state is invalid: {error}")
        metadata = environment["metadata"]
        if not isinstance(metadata, dict) or set(metadata) != {"sha256", "bytes"}:
            fail("dependency image metadata object fields are invalid")
        if (
            not isinstance(metadata["sha256"], str)
            or type(metadata["bytes"]) is not int
            or metadata["bytes"] < 0
        ):
            fail("dependency image metadata object size or digest is invalid")
        metadata_path = _object_path(directory, metadata["sha256"])
        if (
            metadata_path.is_symlink()
            or not metadata_path.is_file()
            or metadata_path.stat().st_size != metadata["bytes"]
            or sha256_file(metadata_path) != metadata["sha256"]
        ):
            fail("dependency image metadata object is missing or mismatched")
        try:
            records = validate_image_metadata(json.loads(metadata_path.read_bytes()))
        except (OSError, ValueError) as error:
            fail(f"dependency image metadata object is invalid: {error}")
        content = json.dumps(records, sort_keys=True, separators=(",", ":")).encode()
        if common.sha256_bytes(content) != environment["state"]["image_content"]:
            fail("dependency image metadata does not match its installed-content identity")
        reference, transport = environment["reference"], environment["transport"]
        if (
            not isinstance(reference, str)
            or not isinstance(transport, str)
            or re.fullmatch(
                re.escape(reference) + r"-dependency-transport-[0-9]+-[0-9a-f]{6}", transport
            )
            is None
        ):
            fail("dependency transport image reference is invalid")
    return value


def _require_checkout(
    manifest: dict[str, Any], inputs: Sequence[DependencyInput], context: dict[str, Any]
) -> None:
    declarations = [asdict(item) for item in sorted(inputs, key=lambda item: item.key)]
    saved = [record["input"] for record in manifest["inputs"]]
    if saved != declarations or manifest["context"] != context:
        fail("dependency snapshot does not match this checkout's declared external inputs")


def restore_inputs(
    directory: Path,
    inputs: Sequence[DependencyInput],
    context: dict[str, Any],
    *,
    cache: Path,
) -> dict[str, Any]:
    """Restore declared cache inputs after validating the complete snapshot."""
    manifest = read_snapshot(directory)
    _require_checkout(manifest, inputs, context)
    _restore_manifest_inputs(directory, manifest, cache)
    return manifest


def _restore_manifest_inputs(directory: Path, manifest: dict[str, Any], cache: Path) -> None:
    """Copy inputs only after their manifest bytes and checkout selection were checked."""
    for record in manifest["inputs"]:
        destination = cache / record["input"]["destination"]
        if destination.exists() or destination.is_symlink():
            if destination.is_symlink() or not destination.is_file():
                fail(f"dependency cache destination is invalid: {destination}")
            if sha256_file(destination) != record["sha256"]:
                fail(f"dependency cache destination contains different bytes: {destination}")
    for record in manifest["inputs"]:
        destination = cache / record["input"]["destination"]
        _copy_verified(_object_path(directory, record["sha256"]), destination, record["sha256"])


def _save_environment(directory: Path, reporter: RunReporter) -> dict[str, Any]:
    lock = images.load_container_lock()
    kern = kern_env.require_kern(lock)
    recipe = images.container_image_recipe_digest(lock)
    image = images.container_image_reference(lock, recipe)
    state = kern_env.current_image_state(kern, image, recipe)
    if state is None:
        fail("current build environment is unavailable; run ./fplinux setup first")
    metadata = _capture_environment_metadata(kern, image, state, reporter)
    metadata_digest = common.sha256_bytes(metadata)
    metadata_path = _object_path(directory, metadata_digest)
    _directory(metadata_path.parent)
    replace_file_atomically(metadata_path, metadata, 0o600)
    transport = kern_env._temporary_image_reference(image, "dependency-transport")  # noqa: SLF001
    try:
        with reporter.stage("environment-save") as stage:
            kern_env._tag_kern_image(kern, image, transport)  # noqa: SLF001 -- preserve source metadata.
            with tempfile.TemporaryDirectory(dir=directory, prefix=".environment-") as temporary:
                archive = Path(temporary) / "environment.tar"
                stage.run(
                    [kern, "save", transport, "-o", str(archive)],
                    cwd=common.ROOT,
                    env=kern_env.kern_environment(),
                    timeout=_ENVIRONMENT_TIMEOUT,
                )
                _require_image_archive(archive, transport)
                if kern_env.current_image_state(kern, image, recipe) != state:
                    fail("source build environment changed during snapshot export")
                digest = sha256_file(archive)
                _copy_verified(archive, _object_path(directory, digest), digest)
                return {
                    "sha256": digest,
                    "bytes": archive.stat().st_size,
                    "reference": image,
                    "transport": transport,
                    "state": state.payload(),
                    "metadata": {"sha256": metadata_digest, "bytes": len(metadata)},
                }
    finally:
        current = kern_env._kern_image_references(kern)  # noqa: SLF001
        kern_env._remove_kern_images(kern, {transport} & set(current))  # noqa: SLF001


def _capture_environment_metadata(
    kern: str, image: str, state: ImageState, reporter: RunReporter
) -> bytes:
    collector = common.ROOT / "scripts/fplinux_cli/image_content.py"
    with reporter.stage("environment-metadata") as stage:
        result = stage.capture(
            [
                kern,
                "box",
                kern_env.kern_box_name("dependency-metadata"),
                "--image",
                image,
                "--pull",
                "never",
                "--read-only",
                "--network",
                "none",
                "--cpus",
                "2",
                "--quiet",
                "--volume",
                f"{collector}:/tmp/fplinux-dependency-content.py:ro",
                "--",
                "python3",
                "-B",
                "/tmp/fplinux-dependency-content.py",  # noqa: S108 -- private read-only box mount.
                "--metadata",
            ],
            cwd=common.ROOT,
            env=kern_env.kern_environment(),
            timeout=300,
        )
        if result.returncode:
            fail("build environment metadata capture failed")
        try:
            records = validate_image_metadata(json.loads(result.stdout))
        except ValueError as error:
            fail(f"build environment metadata is invalid: {error}")
        encoded = json.dumps(records, sort_keys=True, separators=(",", ":")).encode()
        if common.sha256_bytes(encoded) != state.image_content:
            fail("captured build environment metadata does not match its measured content")
        return canonical_json_bytes(records)


def _restore_environment(
    directory: Path, environment: dict[str, Any], reporter: RunReporter
) -> None:
    lock = images.load_container_lock()
    recipe = images.container_image_recipe_digest(lock)
    reference = images.container_image_reference(lock, recipe)
    state = ImageState(**environment["state"])
    if state.container_image_recipe != recipe or environment["reference"] != reference:
        fail("saved build environment does not match this checkout's exact image recipe")
    transport = environment["transport"]
    archive = _object_path(directory, environment["sha256"])
    _require_image_archive(archive, transport)
    kern = kern_env._install_kern(lock, offline=True)  # noqa: SLF001
    staging = kern_env._temporary_image_reference(reference, "staging")  # noqa: SLF001
    try:
        with reporter.stage("environment-load") as stage:
            stage.run(
                [kern, "load", "-i", str(archive)],
                cwd=common.ROOT,
                env=kern_env.kern_environment(),
                timeout=_ENVIRONMENT_TIMEOUT,
            )
        parent = common.ROOT / ".cache/kern"
        _directory(parent)
        with tempfile.TemporaryDirectory(dir=parent, prefix="dependency-restore-") as temporary:
            context = Path(temporary)
            shutil.copyfile(
                common.ROOT / "scripts/fplinux_cli/image_content.py", context / "restore.py"
            )
            shutil.copyfile(
                _object_path(directory, environment["metadata"]["sha256"]),
                context / "metadata.json",
            )
            (context / "Containerfile").write_text(
                f"FROM {transport}\n"
                "COPY restore.py metadata.json /tmp/fplinux-dependency-restore/\n"
                "RUN python3 -B /tmp/fplinux-dependency-restore/restore.py --restore-metadata "
                "/tmp/fplinux-dependency-restore/metadata.json "  # noqa: S108 -- private build layer.
                "&& rm -rf /tmp/fplinux-dependency-restore\n",
                encoding="utf-8",
            )
            with reporter.stage("environment-metadata-restore") as stage:
                stage.run(
                    [kern, "build", "-t", staging, "."],
                    cwd=context,
                    env=kern_env.kern_environment(),
                    timeout=_ENVIRONMENT_TIMEOUT,
                )
        if kern_env.current_image_state(kern, staging, recipe) != state:
            fail("restored build environment failed its exact installed-content check")
        kern_env._publish_staged_kern_image(  # noqa: SLF001 -- same consumer publication boundary.
            kern,
            staging,
            reference,
            lambda candidate: kern_env.current_image_state(kern, candidate, recipe) == state,
        )
        kern_env.publish_current_image_state(kern, reference, recipe, state=state)
    finally:
        current = kern_env._kern_image_references(kern)  # noqa: SLF001
        kern_env._remove_kern_images(kern, {transport, staging} & set(current))  # noqa: SLF001


def _require_image_archive(archive: Path, reference: str) -> None:
    try:
        with tarfile.open(archive, "r:") as bundle:
            members = [
                member for member in bundle if member.name.removeprefix("./") == "manifest.json"
            ]
            if len(members) != 1 or not members[0].isfile():
                fail("Kern export has no unique image manifest")
            source = bundle.extractfile(members[0])
            if source is None:
                fail("Kern export image manifest cannot be read")
            with source:
                manifest = json.load(source)
            if not isinstance(manifest, list) or not any(
                isinstance(item, dict) and reference in item.get("RepoTags", [])
                for item in manifest
            ):
                fail("Kern export does not contain the exact requested image reference")
    except (OSError, tarfile.TarError, ValueError) as error:
        fail(f"Kern export cannot be verified: {error}")


def _checkout_declarations(
    *,
    manifest: dict[str, Any] | None = None,
    offline: bool,
    sources: Sequence[Path] = (),
) -> tuple[list[DependencyInput], dict[str, Any]]:
    inputs = dependency_inputs(common.ROOT)
    context = dependency_context(common.ROOT)
    context["site"] = site_dependency_context(common.ROOT)
    saved_site = None
    if manifest is not None:
        saved_site = [
            DependencyInput(**record["input"])
            for record in manifest["inputs"]
            if record["input"]["purpose"] == "site-python"
        ]
    inputs.extend(
        resolve_site_inputs(
            common.ROOT, offline=offline, saved_inputs=saved_site, source_directories=sources
        )
    )
    return sorted(inputs, key=lambda item: item.key), context


def create_dependencies(
    directory: Path, *, offline: bool, sources: Sequence[Path], inputs_only: bool
) -> None:
    directory = directory.absolute()
    reporter = RunReporter.create("dependencies", target=None, verbose=False)
    with reporter.stage("dependency-selection"):
        inputs, context = _checkout_declarations(offline=offline, sources=sources)
    with reporter.stage("preserve-inputs"):
        manifest = preserve_inputs(
            inputs,
            context,
            directory,
            cache=common.ROOT / ".cache",
            offline=offline,
            sources=sources,
        )
    if not inputs_only:
        manifest["environment"] = _save_environment(directory, reporter)
    current_inputs, current_context = _checkout_declarations(manifest=manifest, offline=True)
    if current_inputs != inputs or current_context != context:
        fail("external dependency declarations changed while the snapshot was being created")
    with reporter.stage("snapshot-verify"):
        publish_snapshot(directory, manifest)
        read_snapshot(directory)
    reporter.finish()
    print(f"Dependency snapshot: {manifest['snapshot']}")


def verify_dependencies(directory: Path) -> None:
    directory = directory.absolute()
    reporter = RunReporter.create("dependencies", target=None, verbose=False)
    with reporter.stage("snapshot-verify"):
        manifest = read_snapshot(directory)
        inputs, context = _checkout_declarations(manifest=manifest, offline=True)
        _require_checkout(manifest, inputs, context)
    reporter.finish()
    print(f"Verified dependency snapshot: {manifest['snapshot']}")


def restore_dependencies(directory: Path, *, inputs_only: bool) -> None:
    directory = directory.absolute()
    reporter = RunReporter.create("dependencies", target=None, verbose=False)
    with reporter.stage("restore-inputs"):
        manifest = read_snapshot(directory)
        inputs, context = _checkout_declarations(manifest=manifest, offline=True)
        _require_checkout(manifest, inputs, context)
        _restore_manifest_inputs(directory, manifest, common.ROOT / ".cache")
        remember_site_inputs(
            common.ROOT, [item for item in inputs if item.purpose == "site-python"]
        )
    if not inputs_only:
        environment = manifest["environment"]
        if environment is None:
            fail(
                "snapshot has no saved build environment; "
                "use --inputs-only to restore its external inputs"
            )
        _restore_environment(directory, environment, reporter)
    reporter.finish()
    print(f"Restored dependency snapshot: {manifest['snapshot']}")
