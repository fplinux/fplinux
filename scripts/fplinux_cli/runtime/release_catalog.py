# SPDX-License-Identifier: GPL-2.0-only
"""Select and assemble one immutable runtime from a universal release archive."""

from __future__ import annotations

import hashlib
import json
import ntpath
import os
import subprocess
import sys
import tempfile
from contextlib import suppress
from dataclasses import dataclass
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.artifacts.card_kit import CARD_APK_INDEX, CARD_APK_KEY
from fplinux_cli.artifacts.release_identity import RELEASE_FIELDS, release_context_from_fields
from fplinux_cli.common import fail, is_sha256, relative_name, sha256_bytes, sha256_file
from fplinux_cli.data_paths import data_root

from . import live
from .bundle import MICROSD_BOOT_MODE, MICROSD_BOOT_PROFILE, TARGET_NAME
from .cli import argument_parser, confirm_card_action, operation_request
from .data_paths import archive_runtime_root
from .operations import (
    BundlePaths,
    CardRequest,
    FileTransfer,
    LiveRequest,
    RunRequest,
    card_selection_error,
    execute_operation,
    file_transfer_available,
    session_selection_error,
)
from .workflow import cancellation_signals, report_runtime_error

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import BinaryIO

    from fplinux_cli.artifacts.release_identity import ReleaseContext

    from .operations import Operation

_BUILD_MANIFEST = "build-manifest.json"
_RUNTIME_MANIFEST = "runtime-manifest.json"
_CHECKSUMS = "SHA256SUMS"
_SHARED_DIRECTORIES = {
    "assets",
    "common",
    "licenses",
    "runtime",
    "scripts",
    "third-party-licenses",
    "tools",
}


@dataclass(frozen=True)
class BundleRecipe:
    """File recipes and native executable modes for one boot profile."""

    files: dict[str, tuple[str, ...]]
    executables: frozenset[str]


@dataclass(frozen=True)
class TargetBundle(BundleRecipe):
    """The default RAM recipe and optional USB Stage0 recipe for a phone."""

    name: str
    microsd: BundleRecipe | None


@dataclass(frozen=True)
class ReleaseCatalog:
    """The release identity and the archive's exact phone model choices."""

    release: ReleaseContext
    targets: dict[str, TargetBundle]


def _portable_path(value: object, field: str) -> str:
    name = relative_name(value, field=field)
    if name == "." or "\\" in name or ntpath.isreserved(name):
        fail(f"{field} must be a portable relative path: {name}")
    return name


def _object_digest(name: str) -> str | None:
    namespace, _, digest = name.partition("/")
    return digest if namespace == "objects" and is_sha256(digest) else None


def _bundle_recipe(target: str, entries: object, executables: object) -> BundleRecipe:
    if not isinstance(entries, dict) or not isinstance(executables, list):
        fail(f"invalid file or executable list for {target}")
    files = {}
    for raw_path, parts in entries.items():
        path = _portable_path(raw_path, f"{target} bundle file")
        if not isinstance(parts, list) or not parts:
            fail(f"missing release archive parts for {target}/{path}")
        names = tuple(_portable_path(part, f"{target} archive member") for part in parts)
        for member in names:
            directory, separator, _rest = member.partition("/")
            if _object_digest(member) is None and (
                not separator or directory not in _SHARED_DIRECTORIES
            ):
                fail(f"invalid release archive member: {member}")
        files[path] = names
    if not {_BUILD_MANIFEST, _RUNTIME_MANIFEST, _CHECKSUMS}.issubset(files):
        fail(f"release catalog has no complete manifest and checksum inventory for {target}")
    for manifest in (_BUILD_MANIFEST, _CHECKSUMS):
        parts = files[manifest]
        if len(parts) != 1 or _object_digest(parts[0]) is None:
            fail(f"{target}/{manifest} must reference one release object")
    if len({path.casefold() for path in files}) != len(files):
        fail(f"release catalog has conflicting file names for {target}")
    modes = frozenset(_portable_path(path, f"{target} executable") for path in executables)
    if not modes.issubset(files) or modes & {_BUILD_MANIFEST, _RUNTIME_MANIFEST, _CHECKSUMS}:
        fail(f"invalid executables for {target}")
    return BundleRecipe(files, modes)


def _target_bundle(target: str, entry: object) -> TargetBundle:
    if not isinstance(entry, dict) or set(entry) - {"name", "files", "executables", "microsd"}:
        fail(f"invalid release catalog entry for {target}")
    name = entry.get("name")
    if not isinstance(name, str) or not name.strip() or not name.isprintable():
        fail(f"invalid phone model name for {target}")
    ram = _bundle_recipe(target, entry.get("files"), entry.get("executables"))
    microsd = None
    if "microsd" in entry:
        card = entry["microsd"]
        if not isinstance(card, dict) or set(card) != {"files", "executables"}:
            fail(f"invalid microSD release catalog entry for {target}")
        microsd = _bundle_recipe(f"{target} microSD", card["files"], card["executables"])
    return TargetBundle(ram.files, ram.executables, name, microsd)


def parse_catalog(data: bytes) -> ReleaseCatalog:
    """Read the same catalog from a ZIP member or an extracted archive."""
    try:
        catalog = json.loads(data)
    except ValueError as error:
        fail(f"invalid targets.json: {error}")
    if not isinstance(catalog, dict) or set(catalog) != RELEASE_FIELDS | {"targets"}:
        fail("targets.json must contain the release identity and targets")
    release = release_context_from_fields(catalog)
    entries = catalog["targets"]
    if not isinstance(entries, dict) or not entries:
        fail("targets.json must list at least one phone model")
    targets = {}
    for target, entry in entries.items():
        if TARGET_NAME.fullmatch(target) is None:
            fail(f"invalid release target ID: {target}")
        _portable_path(target, "release target ID")
        targets[target] = _target_bundle(target, entry)
    return ReleaseCatalog(release, targets)


def load_catalog(archive_root: Path) -> ReleaseCatalog:
    """Read the model choices without touching managed state or USB."""
    return parse_catalog((archive_root / "targets.json").read_bytes())


def target_runtime_root(managed_root: Path, archive_root: Path, target: str) -> Path:
    """Keep credentials and logs scoped to the exact archive and phone model."""
    return archive_runtime_root(managed_root, archive_root) / "targets" / target


def target_recipe(catalog: ReleaseCatalog, target: str, *, microsd: bool = False) -> BundleRecipe:
    """Select exactly the model and boot recipe requested by the launcher."""
    board = catalog.targets.get(target)
    if board is None:
        fail(f"unknown release target: {target}")
    if not microsd:
        return board
    if board.microsd is None:
        fail(f"{target} has no system microSD boot in this archive")
    return board.microsd


def _part_path(archive_root: Path, member: str) -> Path:
    path = archive_root / member
    if not path.is_file() or path.is_symlink():
        fail(f"the extracted release is missing archive member {member}")
    return path


def _read_object(archive_root: Path, name: str) -> bytes:
    data = _part_path(archive_root, name).read_bytes()
    if sha256_bytes(data) != _object_digest(name):
        fail(f"SHA256 mismatch in archive member {name}")
    return data


def read_bundle_manifest(
    data: bytes, name: str, *, target: str, release: ReleaseContext, profile: str | None
) -> dict[str, Any]:
    """Read a bundle manifest and verify that it belongs to its archive selection."""
    try:
        manifest = json.loads(data)
    except ValueError as error:
        fail(f"invalid {name} for {target}: {error}")
    if (
        not isinstance(manifest, dict)
        or manifest.get("target") != target
        or release_context_from_fields(manifest) != release
        or manifest.get("profile") != profile
    ):
        fail(
            f"{name} does not match release {release.release_id}, "
            f"phone model {target} and boot profile"
        )
    return manifest


def read_target_manifest(
    archive_root: Path, catalog: ReleaseCatalog, target: str, *, microsd: bool = False
) -> dict[str, Any]:
    """Read a model's runtime metadata without assembling its image."""
    parts = target_recipe(catalog, target, microsd=microsd).files[_RUNTIME_MANIFEST]
    data = b"".join(_part_path(archive_root, name).read_bytes() for name in parts)
    return read_bundle_manifest(
        data,
        _RUNTIME_MANIFEST,
        target=target,
        release=catalog.release,
        profile=MICROSD_BOOT_PROFILE if microsd else None,
    )


def _copy_bundle_file(archive_root: Path, parts: tuple[str, ...], output: BinaryIO) -> str:
    digest = hashlib.sha256()
    for part in parts:
        with _part_path(archive_root, part).open("rb") as source:
            while chunk := source.read(1024 * 1024):
                output.write(chunk)
                digest.update(chunk)
    return digest.hexdigest()


def _stage_bundle_files(
    archive_root: Path,
    destination: Path,
    recipe: BundleRecipe,
    records: dict[str, Any],
    *,
    omitted: set[str],
) -> None:
    for relative, record in records.items():
        mode = 0o755 if relative in recipe.executables else 0o644
        if (
            not isinstance(record, dict)
            or record.get("mode") != mode
            or not is_sha256(record.get("sha256"))
            or type(record.get("size")) is not int
        ):
            fail(f"invalid build manifest file record for {relative}")
        if relative in omitted:
            continue
        path = destination / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as stream:
            actual = _copy_bundle_file(archive_root, recipe.files[relative], stream)
            if actual != record["sha256"] or stream.tell() != record["size"]:
                fail(f"{relative} SHA256 or size differs from the build manifest")
        path.chmod(mode)


def _verify_live_host_files(
    archive_root: Path, recipe: BundleRecipe, records: dict[str, Any], *, omitted: set[str]
) -> None:
    """Check the outer files still used directly when a board bundle is cached."""
    for relative, parts in recipe.files.items():
        part = parts[0]
        if (
            relative in omitted
            or len(parts) != 1
            or not part.startswith(("common/", "runtime/", "scripts/", "tools/"))
        ):
            continue
        path = _part_path(archive_root, part)
        record = records[relative]
        if not isinstance(record, dict) or sha256_file(path) != record.get("sha256"):
            fail(f"{part} SHA256 differs from the selected build manifest")


def materialize_bundle(  # noqa: PLR0913 -- boot selection and card assets are independent inputs.
    archive_root: Path,
    managed_root: Path,
    catalog: ReleaseCatalog,
    target: str,
    *,
    microsd: bool = False,
    include_card: bool = True,
) -> Path:
    """Publish the selected bundle atomically under its original inventory's digest."""
    recipe = target_recipe(catalog, target, microsd=microsd)
    profile = MICROSD_BOOT_PROFILE if microsd else None
    manifest_object = recipe.files[_BUILD_MANIFEST][0]
    manifest_data = _read_object(archive_root, manifest_object)
    manifest = read_bundle_manifest(
        manifest_data,
        _BUILD_MANIFEST,
        target=target,
        release=catalog.release,
        profile=profile,
    )
    records = manifest.get("files")
    if not isinstance(records, dict) or records.keys() != recipe.files.keys() - {
        _BUILD_MANIFEST,
        _CHECKSUMS,
    }:
        fail(f"release catalog files differ from the build manifest for {target}")

    checksum_object = recipe.files[_CHECKSUMS][0]
    checksum_data = _read_object(archive_root, checksum_object)
    inventory = sha256_bytes(f"{manifest_object}\n{checksum_object}".encode())
    omitted = set() if include_card else {name for name in records if name.startswith("card/")}
    bundles = managed_root / "runtime/archive-bundles"
    # Keep the original build generation for reader reuse, with a separate cache entry
    # so an install always requires the complete bundle.
    bundle = bundles / (f"{inventory}-reader" if omitted else inventory)
    if bundle.is_dir():
        _verify_live_host_files(archive_root, recipe, records, omitted=omitted)
        return bundle

    bundles.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=bundles, prefix=".stage-") as temporary:
        staged = Path(temporary) / "bundle"
        staged.mkdir()
        _stage_bundle_files(archive_root, staged, recipe, records, omitted=omitted)
        read_bundle_manifest(
            (staged / _RUNTIME_MANIFEST).read_bytes(),
            _RUNTIME_MANIFEST,
            target=target,
            release=catalog.release,
            profile=profile,
        )
        (staged / _BUILD_MANIFEST).write_bytes(manifest_data)
        (staged / _BUILD_MANIFEST).chmod(0o644)
        (staged / _CHECKSUMS).write_bytes(checksum_data)
        (staged / _CHECKSUMS).chmod(0o644)
        try:
            staged.rename(bundle)
        except OSError:
            # Windows and Linux report different errors when another launcher published first.
            if not bundle.is_dir():
                raise
    return bundle


def _choose_target(catalog: ReleaseCatalog) -> str | None:
    ordered = sorted(
        catalog.targets, key=lambda target: (catalog.targets[target].name.casefold(), target)
    )
    choices = {str(number): target for number, target in enumerate(ordered, start=1)}
    print("Select your phone model:")
    for number, target in choices.items():
        print(f"  {number}. {catalog.targets[target].name} ({target})")
    print("  0. Exit")
    while True:
        selection = input("Phone model: ").strip()
        if selection == "0":
            return None
        if selection in choices:
            return choices[selection]
        if selection in catalog.targets:
            return selection
        print("Enter a model number or its target ID.")


def _choose_action(
    target: str, *, install_card: bool, boot_card: bool, change_card: bool, copy_files: bool
) -> RunRequest | LiveRequest | CardRequest | None:
    choices: dict[str, tuple[str, RunRequest | LiveRequest | CardRequest | None]] = {
        "1": ("Start in RAM", RunRequest(target=target))
    }
    if install_card:
        choices["2"] = ("Install on microSD", CardRequest(target=target))
    if boot_card:
        choices["3"] = ("Start from microSD", RunRequest(target=target, boot=MICROSD_BOOT_MODE))
    if change_card:
        choices["4"] = ("Update microSD", CardRequest(target=target, action="update"))
        choices["5"] = (
            "Select a previous microSD build",
            CardRequest(target=target, action="rollback"),
        )
    if boot_card:
        choices["6"] = (
            "Shut down the running microSD system",
            LiveRequest(target=target, shutdown=True),
        )
    if copy_files:
        choices["7"] = ("Upload a file to the running phone", None)
        choices["8"] = ("Pull a file from the running phone", None)
    for number, (label, _request) in choices.items():
        print(f"  {number}. {label}")
    print("  0. Exit")
    while True:
        action = input("Action: ").strip()
        if action == "0":
            return None
        if action == "5" and change_card:
            build_id = input("Previous confirmed build ID: ").strip()
            return CardRequest(target=target, action="rollback", build_id=build_id)
        if copy_files and action in {"7", "8"}:
            boot = None
            if boot_card:
                system = input("Running system (1. RAM, 2. microSD): ").strip()
                if system not in {"1", "2"}:
                    print("Enter 1 for RAM or 2 for microSD.")
                    continue
                if system == "2":
                    boot = MICROSD_BOOT_MODE
            local = input("File on this computer: ")
            remote = input("Absolute file path on the phone: ")
            if not local or not remote:
                print("Enter both file paths.")
                continue
            return LiveRequest(
                target=target,
                boot=boot,
                transfer=FileTransfer(
                    direction="upload" if action == "7" else "pull",
                    local=Path(local),
                    remote=remote,
                ),
            )
        if action in choices:
            return choices[action][1]
        print(f"Enter one of {', '.join(choices)} or 0.")


def _connection_files(
    archive_root: Path, catalog: ReleaseCatalog, target: str, *, microsd: bool
) -> Callable[[str], Path]:
    """Read the verified native files for discovery without expanding the phone image."""
    recipe = target_recipe(catalog, target, microsd=microsd)
    build_object = recipe.files[_BUILD_MANIFEST][0]
    build = read_bundle_manifest(
        _read_object(archive_root, build_object),
        _BUILD_MANIFEST,
        target=target,
        release=catalog.release,
        profile=MICROSD_BOOT_PROFILE if microsd else None,
    )
    records = build.get("files")
    if not isinstance(records, dict):
        fail(f"missing build manifest files for {target}")

    def checked_path(relative: str) -> Path:
        parts = recipe.files.get(relative, ())
        record = records.get(relative)
        if len(parts) != 1 or not isinstance(record, dict):
            fail(f"missing native connection file for {target}: {relative}")
        if relative.startswith("common/windows/") and parts[0] != relative:
            fail(f"invalid native command file for {target}: {relative}")
        path = _part_path(archive_root, parts[0])
        if sha256_file(path) != record.get("sha256") or path.stat().st_size != record.get("size"):
            fail(f"{relative} SHA256 or size differs from the build manifest")
        return path

    for script in ("user.ps1", "acl.ps1", "devices.ps1"):
        checked_path(f"common/windows/{script}")
    return checked_path


def _selects_microsd(request: Operation) -> bool:
    return isinstance(request, RunRequest | LiveRequest) and (
        request.boot == MICROSD_BOOT_MODE
        or (
            isinstance(request, LiveRequest) and (request.shutdown or request.card_cid is not None)
        )
    )


def _archive_connection(
    archive_root: Path,
    managed_root: Path,
    catalog: ReleaseCatalog,
    request: LiveRequest,
    *,
    data_override: Path | None,
) -> tuple[Any, dict[str, Any]]:
    """Find a single saved USB binding in the selected archive's state directories."""
    candidates = []
    microsd = _selects_microsd(request)
    for target, board in sorted(catalog.targets.items()):
        if microsd and board.microsd is None:
            continue
        scope = data_override or target_runtime_root(managed_root, archive_root, target)
        session_root = scope / "session"
        if not session_root.is_dir():
            continue
        if os.name == "nt":
            host_file = _connection_files(archive_root, catalog, target, microsd=microsd)
            runtime = read_bundle_manifest(
                host_file(_RUNTIME_MANIFEST).read_bytes(),
                _RUNTIME_MANIFEST,
                target=target,
                release=catalog.release,
                profile=MICROSD_BOOT_PROFILE if microsd else None,
            )
            candidates.extend(
                live.load_connections(
                    target,
                    session_root,
                    runtime=runtime,
                    host_file=host_file,
                    shared_root=data_override is not None,
                )
            )
        else:
            candidates.extend(live.load_connections(target, session_root))
    return live.select_connection(candidates)


def archive_operation_paths(
    archive_root: Path,
    catalog: ReleaseCatalog,
    request: Operation,
    *,
    data_override: Path | None = None,
    log_override: Path | None = None,
) -> BundlePaths:
    """Resolve a typed request against this release and its exact installation scope."""
    if isinstance(request, RunRequest | LiveRequest) and (
        error := session_selection_error(request)
    ):
        fail(error)
    managed_root = data_root(archive_root)
    data = data_override.resolve() if data_override is not None else None
    target = request.target
    connection = None
    if target is None:
        if not isinstance(request, LiveRequest):
            fail("Select a phone target")
        connection = _archive_connection(
            archive_root, managed_root, catalog, request, data_override=data
        )
        target = str(connection[1]["target"])
    scope = target_runtime_root(managed_root, archive_root, target)
    logs = log_override.resolve() if log_override is not None else scope / "logs"
    bundle = materialize_bundle(
        archive_root,
        managed_root,
        catalog,
        target,
        microsd=_selects_microsd(request),
        include_card=not (isinstance(request, CardRequest) and request.action == "inspect"),
    )
    return BundlePaths(bundle, data if data is not None else scope, logs, connection=connection)


def main(*, archive_root: Path) -> None:
    """Choose a target explicitly or at the terminal, then run the shared runtime action."""
    interactive = len(sys.argv) == 1
    may_confirm = interactive or sys.stdin.isatty()
    try:
        with cancellation_signals():
            catalog = load_catalog(archive_root)
            card_targets = {
                target
                for target, board in catalog.targets.items()
                if "card/layout.json" in board.files
            }
            microsd_targets = {
                target for target, board in catalog.targets.items() if board.microsd is not None
            }
            update_targets = {
                target
                for target in card_targets
                if {f"card/{CARD_APK_INDEX}", f"card/{CARD_APK_KEY}"}
                <= catalog.targets[target].files.keys()
            }
            data_override = log_override = None
            request: Operation | None
            if interactive:
                target = _choose_target(catalog)
                if target is None:
                    return
                copy_files = file_transfer_available()
                request = (
                    _choose_action(
                        target,
                        install_card=target in card_targets,
                        boot_card=target in microsd_targets,
                        change_card=target in update_targets,
                        copy_files=copy_files,
                    )
                    if copy_files or target in card_targets or target in microsd_targets
                    else RunRequest(target=target)
                )
                if request is None:
                    return
            else:
                parser = argument_parser(
                    targets=sorted(catalog.targets),
                    card_targets=card_targets,
                )
                options = parser.parse_args()
                request = operation_request(parser, options)
                data_override, log_override = options.data_root, options.log_root
                if (
                    _selects_microsd(request)
                    and request.target is not None
                    and request.target not in microsd_targets
                ):
                    parser.error(f"{request.target} has no system microSD boot in this archive")
            if isinstance(request, CardRequest):
                if (
                    request.action in {"update", "rollback"}
                    and request.target not in update_targets
                ):
                    fail(f"{request.target} has no system microSD update kit in this archive")
                error = card_selection_error(
                    request.action,
                    request.erase_card,
                    request.card_cid,
                    request.build_id,
                    may_confirm=may_confirm,
                )
                if error is not None:
                    if not interactive:
                        parser.error(error)
                    fail(error)
            paths = archive_operation_paths(
                archive_root,
                catalog,
                request,
                data_override=data_override,
                log_override=log_override,
            )
    except EOFError:
        print("FPLinux: No model selected. Use run TARGET to start.", file=sys.stderr)
        raise SystemExit(2) from None
    except KeyboardInterrupt:
        print("FPLinux: Cancelled.", file=sys.stderr)
        raise SystemExit(130) from None
    except subprocess.TimeoutExpired as error:
        _, code = report_runtime_error(error, None)
        raise SystemExit(code) from None
    except (OSError, ValueError) as error:
        fail(str(error))
    apks = paths.bundle / "apks"
    if isinstance(request, RunRequest) and apks.is_dir():
        print(f"Optional APKs: {apks}", flush=True)
    consent = (
        partial(confirm_card_action, action=request.action)
        if isinstance(request, CardRequest) and may_confirm
        else None
    )
    status = execute_operation(request, paths, confirm_card=consent)
    keep_menu_open = isinstance(request, CardRequest) or (
        isinstance(request, LiveRequest) and (request.shutdown or request.transfer is not None)
    )
    if interactive and status == 0 and keep_menu_open:
        with suppress(EOFError, KeyboardInterrupt):
            input("Press Enter to close the launcher. ")
    raise SystemExit(status)
