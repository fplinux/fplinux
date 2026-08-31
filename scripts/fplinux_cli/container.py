# SPDX-License-Identifier: GPL-2.0-only
"""Manage the single project-local Kern build environment."""

from __future__ import annotations

import json
import os
import platform
import re
import secrets
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import urllib.request
from pathlib import Path, PurePath, PurePosixPath
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Callable

from . import alpine_state
from .checkreceipts import (
    CheckReceiptRecipe,
    check_closure_entries_digest,
    publish_success_receipt,
    receipt_matches,
)
from .common import ROOT, fail, sha256_file
from .config import (
    check_orchestration_recipe_digest,
    container_base_image_reference,
    container_image_build_arguments,
    container_image_recipe_digest,
    container_image_reference,
    discover_profiles,
    discover_targets,
    load_container_lock,
)
from .image_state import ImageState, ImageStateError, load_image_state, publish_image_state
from .output import RunReporter
from .prune import discard_superseded_profile_logs
from .source_formats import shell_dialect
from .workspace import (
    WorkspaceFile,
    WorkspaceSnapshot,
    discard_staged_quality_workspace_snapshot,
    quality_workspace_snapshot,
    stage_quality_workspace_snapshot,
)

CHECK_SCOPES = (
    "repository",
    "source",
    "container",
    "metadata",
    "docs",
    "spelling",
    "secrets",
    "licenses",
    "python",
    "shell",
    "alpine",
    "c",
    "kernel",
)
SOURCE_CHECK_SCOPES = CHECK_SCOPES[1:-1]
GIT_HOOKS_PATH = ".githooks"
_CHECK_GIT_TIMEOUT = 5 * 60
_GIT_HOOK_TIMEOUT = 60
_KERN_PROBE_TIMEOUT = 60
_COMMIT_MESSAGE_TIMEOUT = 5 * 60
_CONTAINER_SETUP_TIMEOUT = 2 * 60 * 60
_SOURCE_CHECK_TIMEOUT = 2 * 60 * 60
_KERNEL_PREPARE_TIMEOUT = 90 * 60
_KERNEL_ANALYSIS_TIMEOUT = 90 * 60
_QUOTED_C_INCLUDE = re.compile(rb'^\s*#\s*include\s*"([^"\n]+)"', re.MULTILINE)
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_PRETTIER_CONFIGURATION_NAMES = frozenset(
    {
        ".prettierrc",
        ".prettierrc.cjs",
        ".prettierrc.cts",
        ".prettierrc.js",
        ".prettierrc.json",
        ".prettierrc.json5",
        ".prettierrc.mjs",
        ".prettierrc.mts",
        ".prettierrc.toml",
        ".prettierrc.ts",
        ".prettierrc.yaml",
        ".prettierrc.yml",
        "prettier.config.cjs",
        "prettier.config.cts",
        "prettier.config.js",
        "prettier.config.mjs",
        "prettier.config.mts",
        "prettier.config.ts",
    }
)
_EXECUTABLE_PRETTIER_CONFIGURATION_NAMES = frozenset(
    name
    for name in _PRETTIER_CONFIGURATION_NAMES
    if Path(name).suffix in {".cjs", ".cts", ".js", ".mjs", ".mts", ".ts"}
)
_SOURCE_CHECK_COMMAND = ("python3", "/workspace/scripts/check.py")
_KERNEL_CHECK_COMMANDS = (
    ("python3", "-m", "fplinux_cli.kernelcheck", "prepare"),
    ("python3", "-m", "fplinux_cli.kernelcheck", "check"),
)
_CHECK_IMPLEMENTATION = frozenset(
    {
        "scripts/check.py",
        "scripts/fplinux_cli/__init__.py",
        "scripts/fplinux_cli/alpine_state.py",
        "scripts/fplinux_cli/common.py",
        "scripts/fplinux_cli/config.py",
        "scripts/fplinux_cli/identity.py",
        "scripts/fplinux_cli/identity_codegen.py",
        "scripts/fplinux_cli/output.py",
        "scripts/fplinux_cli/source_formats.py",
    }
)
_KERNEL_IMPLEMENTATION = frozenset(
    {
        "scripts/fplinux_cli/__init__.py",
        "scripts/fplinux_cli/alpine_builder.py",
        "scripts/fplinux_cli/alpine_state.py",
        "scripts/fplinux_cli/build_env.py",
        "scripts/fplinux_cli/builder.py",
        "scripts/fplinux_cli/bundle_state.py",
        "scripts/fplinux_cli/common.py",
        "scripts/fplinux_cli/config.py",
        "scripts/fplinux_cli/device_state.py",
        "scripts/fplinux_cli/device_tree.py",
        "scripts/fplinux_cli/identity.py",
        "scripts/fplinux_cli/identity_codegen.py",
        "scripts/fplinux_cli/kbuild_state.py",
        "scripts/fplinux_cli/kernelcheck.py",
        "scripts/fplinux_cli/linux_state.py",
        "scripts/fplinux_cli/output.py",
    }
)


def _ensure_project_directory(path: Path) -> Path:
    """Create one exact project-owned directory without following a symlink."""
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        fail(f"invalid project runtime directory: {path}")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    return path


def kern_environment() -> dict[str, str]:
    """Confine persistent Kern state while retaining the host runtime directory."""
    root = _ensure_project_directory(ROOT / ".cache/kern")
    environment = os.environ.copy()
    for variable, name in (
        ("XDG_CACHE_HOME", "cache"),
        ("XDG_DATA_HOME", "data"),
        ("XDG_CONFIG_HOME", "config"),
    ):
        environment[variable] = str(_ensure_project_directory(root / name))
    return environment


def _kern_path() -> Path:
    return ROOT / ".cache/tools/kern/kern"


def kern_available(lock: dict[str, Any] | None = None) -> bool:
    """Return whether the exact pinned Kern binary is already project-local."""
    if lock is None:
        lock = load_container_lock()
    executable = _kern_path()
    return (
        not executable.is_symlink()
        and executable.is_file()
        and sha256_file(executable) == lock["kern"]["binary_sha256"]
    )


def require_kern(lock: dict[str, Any] | None = None) -> str:
    """Return the exact project-local Kern binary."""
    if lock is None:
        lock = load_container_lock()
    if not kern_available(lock):
        fail("Kern is not ready for this checkout; run ./fplinux setup online first")
    return str(_kern_path())


def _download_locked_file(
    url: str,
    digest: str,
    destination: Path,
) -> Path:
    """Download one exact HTTPS runtime input atomically into the project cache."""
    if not url.startswith("https://"):
        fail("runtime download URL must use HTTPS")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() or (destination.exists() and not destination.is_file()):
        fail(f"invalid runtime download path: {destination}")
    if destination.is_file() and sha256_file(destination) == digest:
        return destination
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent,
            prefix=f".{destination.name}.",
            delete=False,
        ) as output:
            temporary = Path(output.name)
            request = urllib.request.Request(  # noqa: S310 -- HTTPS is required above.
                url,
                headers={"User-Agent": "FPLinux"},
            )
            with urllib.request.urlopen(  # noqa: S310 -- HTTPS is required above.
                request,
                timeout=60,
            ) as response:
                shutil.copyfileobj(response, output)
        actual = sha256_file(temporary)
        if actual != digest:
            fail(f"runtime download SHA256 mismatch: expected {digest}, received {actual}")
        temporary.replace(destination)
        temporary = None
    except OSError as error:
        fail(f"runtime download failed: {error}")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return destination


def _install_kern(lock: dict[str, Any]) -> str:
    """Install the pinned static Kern binary under the project cache."""
    if kern_available(lock):
        return str(_kern_path())
    kern_lock = lock["kern"]
    archive = _download_locked_file(
        kern_lock["archive_url"],
        kern_lock["archive_sha256"],
        ROOT / ".cache/downloads/kern/kern.tar.gz",
    )
    destination = _kern_path()
    _ensure_project_directory(destination.parent)
    temporary: Path | None = None
    try:
        with tarfile.open(archive, "r:gz") as bundle:
            try:
                member = bundle.getmember("kern")
            except KeyError:
                fail("pinned Kern archive contains no kern binary")
            if not member.isfile():
                fail("pinned Kern archive kern entry is not a regular file")
            source = bundle.extractfile(member)
            if source is None:
                fail("could not read kern from the pinned release archive")
            with tempfile.NamedTemporaryFile(
                dir=destination.parent,
                prefix=".kern.",
                delete=False,
            ) as output:
                temporary = Path(output.name)
                shutil.copyfileobj(source, output)
        actual = sha256_file(temporary)
        if actual != kern_lock["binary_sha256"]:
            fail(
                "Kern binary SHA256 mismatch: "
                f"expected {kern_lock['binary_sha256']}, received {actual}"
            )
        temporary.chmod(0o755)
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return str(destination)


def kern_box_name(label: str) -> str:
    """Return one collision-resistant foreground box name owned by this invocation."""
    normalized = re.sub(r"[^a-z0-9-]+", "-", label.lower()).strip("-") or "task"
    return f"fplinux-{normalized}-{os.getpid()}-{secrets.token_hex(3)}"


def _kern_image_references(kern: str) -> frozenset[str]:
    """Return the exact images in this checkout's isolated Kern store."""
    try:
        result = subprocess.run(
            [kern, "images", "--json"],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=_KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern image inventory timed out after {_KERN_PROBE_TIMEOUT}s")
    if result.returncode:
        fail(result.stderr.strip() or "Kern image inventory failed")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        fail("Kern image inventory is not valid JSON")
    if not isinstance(payload, list):
        fail("Kern image inventory root is invalid")
    references: set[str] = set()
    for entry in payload:
        reference = entry.get("image") if isinstance(entry, dict) else None
        if not isinstance(reference, str) or not reference:
            fail("Kern image inventory entry is invalid")
        references.add(reference)
    return frozenset(references)


def _remove_kern_images(kern: str, references: set[str]) -> None:
    """Remove exact provider-owned image references, never the whole Kern store."""
    if not references:
        return
    try:
        result = subprocess.run(
            [kern, "rmi", *sorted(references)],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=_KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern image removal timed out after {_KERN_PROBE_TIMEOUT}s")
    if result.returncode:
        fail(result.stderr.strip() or "Kern image removal failed")


def _prune_kern_build_history(kern: str) -> None:
    """Remove provider build records after FPLinux has retained its own setup logs."""
    try:
        result = subprocess.run(
            [kern, "build", "prune", "--keep", "0"],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=_KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern build-history pruning timed out after {_KERN_PROBE_TIMEOUT}s")
    if result.returncode:
        fail(result.stderr.strip() or "Kern build-history pruning failed")


def _discard_obsolete_kern_images(
    kern: str,
    lock: dict[str, Any],
    image_recipe: str,
) -> None:
    """Keep only the exact current FPLinux base and build images."""
    keep = {
        container_base_image_reference(lock),
        container_image_reference(lock, image_recipe),
    }
    prefixes = (
        f"{lock['oci']['base_repository']}:",
        f"{lock['oci']['repository']}:",
    )
    stale = {
        reference
        for reference in _kern_image_references(kern)
        if reference not in keep and reference.startswith(prefixes)
    }
    _remove_kern_images(kern, stale)


def _discard_transient_kern_images(kern: str, lock: dict[str, Any]) -> None:
    """Remove only abandoned FPLinux staging and backup tags from an earlier invocation."""
    prefixes = (
        f"{lock['oci']['base_repository']}:",
        f"{lock['oci']['repository']}:",
    )
    stale = {
        reference
        for reference in _kern_image_references(kern)
        if reference.startswith(prefixes) and ("-staging-" in reference or "-backup-" in reference)
    }
    _remove_kern_images(kern, stale)


def _temporary_image_reference(image: str, role: str) -> str:
    """Return one invocation-owned staging or backup tag beside a final image tag."""
    return f"{image}-{role}-{os.getpid()}-{secrets.token_hex(3)}"


def _tag_kern_image(kern: str, source: str, destination: str) -> None:
    """Apply one bounded provider tag operation."""
    try:
        result = subprocess.run(
            [kern, "tag", source, destination],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=_KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern image publication timed out after {_KERN_PROBE_TIMEOUT}s")
    if result.returncode:
        fail(result.stderr.strip() or "Kern image publication failed")


def _publish_staged_kern_image(
    kern: str,
    staging: str,
    destination: str,
    validate: Callable[[str], bool],
) -> None:
    """Replace one consumer tag while retaining a restorable last-good image."""
    existing = destination in _kern_image_references(kern)
    backup = _temporary_image_reference(destination, "backup") if existing else None
    if backup is not None:
        _tag_kern_image(kern, destination, backup)
    try:
        _tag_kern_image(kern, staging, destination)
        if not validate(destination):
            fail("published Kern image failed its exact validation")
    except BaseException:
        if backup is not None:
            _tag_kern_image(kern, backup, destination)
        else:
            current = _kern_image_references(kern)
            _remove_kern_images(kern, {destination} & set(current))
        raise
    finally:
        current = _kern_image_references(kern)
        disposable = {staging}
        if backup is not None:
            disposable.add(backup)
        _remove_kern_images(kern, disposable & set(current))


def _image_metadata(kern: str, image: str) -> tuple[str, str] | None:
    """Read FPLinux's static recipe and generation through the Kern runtime."""
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("image-probe"),
                "--image",
                image,
                "--pull",
                "never",
                "--read-only",
                "--network",
                "none",
                "--no-uid-range",
                "--quiet",
                "--",
                "cat",
                "/etc/fplinux-image-state",
            ],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=_KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern image lookup timed out after {_KERN_PROBE_TIMEOUT}s")
    lines = result.stdout.splitlines()
    if result.returncode != 0 or len(lines) != 2:
        return None
    recipe, generation = lines
    if _SHA256.fullmatch(recipe) is None:
        return None
    if _SHA256.fullmatch(generation) is None:
        return None
    return recipe, generation


def image_generation(kern: str, image: str) -> str | None:
    """Return the exact build generation embedded in one project-built Kern image."""
    metadata = _image_metadata(kern, image)
    return None if metadata is None else metadata[1]


def current_image_state(
    kern: str,
    image: str,
    image_recipe: str | None = None,
) -> ImageState | None:
    """Read one valid current recipe and generation with a single Kern probe."""
    metadata = _image_metadata(kern, image)
    if metadata is None:
        return None
    if image_recipe is None:
        image_recipe = container_image_recipe_digest()
    recipe, generation = metadata
    if recipe != image_recipe:
        return None
    try:
        return ImageState(recipe, generation)
    except ImageStateError:
        return None


def image_ready(
    kern: str,
    image: str,
    *,
    image_recipe: str | None = None,
) -> bool:
    return current_image_state(kern, image, image_recipe) is not None


def publish_current_image_state(
    kern: str,
    image: str,
    image_recipe: str,
    *,
    state: ImageState | None = None,
) -> ImageState:
    """Persist the marker of one image already checked against its static recipe."""
    if state is None:
        state = current_image_state(kern, image, image_recipe)
    if state is None or state.container_image_recipe != image_recipe:
        fail("current build image has no valid generation")
    try:
        publish_image_state(ROOT / ".cache", state)
    except ImageStateError as error:
        fail(f"could not publish host image state: {error}")
    return state


def _run_git_hook_command(git: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    """Run one bounded Git query or mutation for hook ownership."""
    try:
        return subprocess.run(
            [git, *arguments],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            timeout=_GIT_HOOK_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Git hook configuration timed out after {_GIT_HOOK_TIMEOUT}s")


def install_git_hooks() -> None:
    """Select the repository-owned hooks for this Git checkout."""
    git = shutil.which("git")
    if git is None:
        return
    checkout = _run_git_hook_command(git, "rev-parse", "--show-toplevel")
    if checkout.returncode:
        return
    if Path(checkout.stdout.strip()).resolve() != ROOT:
        fail("Git reports a different repository root")
    configured = _run_git_hook_command(
        git,
        "config",
        "--local",
        "--get",
        "core.hooksPath",
    )
    if configured.returncode not in {0, 1}:
        fail("could not read the local Git hooks path")
    hooks_path = configured.stdout.strip()
    if hooks_path:
        configured_path = Path(hooks_path)
        if not configured_path.is_absolute():
            configured_path = ROOT / configured_path
        if configured_path.resolve() != (ROOT / GIT_HOOKS_PATH).resolve():
            fail(f"core.hooksPath is already set to {hooks_path}")
    if not hooks_path:
        updated = _run_git_hook_command(
            git,
            "config",
            "--local",
            "core.hooksPath",
            GIT_HOOKS_PATH,
        )
        if updated.returncode:
            fail("could not configure the local Git hooks path")
    print(f"Git hooks are ready: {GIT_HOOKS_PATH}")


def _alpine_tar_filter(member: tarfile.TarInfo, destination: str) -> tarfile.TarInfo | None:
    """Apply Python's data filter while preserving safe absolute Alpine symlinks."""
    if member.issym() or member.islnk():
        target = PurePosixPath(member.linkname)
        if target.is_absolute():
            if ".." in target.parts:
                fail(f"Alpine minirootfs link escapes the root: {member.name}")
            relative_target = target.as_posix().lstrip("/")
            filtered = tarfile.data_filter(member.replace(linkname=relative_target), destination)
            if filtered is None:
                return None
            return filtered.replace(linkname=member.linkname)
    return tarfile.data_filter(member, destination)


def _base_image_ready(kern: str, lock: dict[str, Any], image: str | None = None) -> bool:
    """Require the local base tag to expose the exact locked release and rootfs marker."""
    oci = lock["oci"]
    if image is None:
        image = container_base_image_reference(lock)
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("base-probe"),
                "--image",
                image,
                "--pull",
                "never",
                "--read-only",
                "--network",
                "none",
                "--no-uid-range",
                "--quiet",
                "--",
                "/bin/cat",
                "/etc/alpine-release",
                "/etc/fplinux-base-rootfs-sha256",
            ],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=_KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern base image lookup timed out after {_KERN_PROBE_TIMEOUT}s")
    expected = f"{oci['base_release']}\n{oci['base_rootfs_sha256']}\n"
    return result.returncode == 0 and result.stdout == expected


def _build_base_image(kern: str, reporter: RunReporter, lock: dict[str, Any]) -> None:
    """Build one local Kern base from the exact official Alpine minirootfs archive."""
    oci = lock["oci"]
    image = container_base_image_reference(lock)
    staging_image = _temporary_image_reference(image, "staging")
    archive = _download_locked_file(
        oci["base_rootfs_url"],
        oci["base_rootfs_sha256"],
        ROOT / ".cache/downloads/kern/alpine-minirootfs.tar.gz",
    )
    temporary_parent = _ensure_project_directory(ROOT / ".cache/kern")
    with tempfile.TemporaryDirectory(dir=temporary_parent, prefix="base-build-") as temporary:
        context = Path(temporary)
        rootfs = context / "rootfs"
        rootfs.mkdir()
        with tarfile.open(archive, "r:gz") as bundle:
            bundle.extractall(  # noqa: S202 -- every member passes the data-derived filter above.
                rootfs,
                filter=_alpine_tar_filter,
            )
        marker = rootfs / "etc/fplinux-base-rootfs-sha256"
        marker.write_text(f"{oci['base_rootfs_sha256']}\n", encoding="utf-8")
        recipe = context / "Containerfile"
        recipe.write_text("FROM scratch\nCOPY rootfs/ /\n", encoding="utf-8")
        with reporter.stage("container-base") as stage:
            stage.run(
                [
                    kern,
                    "build",
                    "-t",
                    staging_image,
                    "-f",
                    str(recipe),
                    str(context),
                ],
                cwd=ROOT,
                env=kern_environment(),
                timeout=_CONTAINER_SETUP_TIMEOUT,
            )
    if not _base_image_ready(kern, lock, staging_image):
        fail("Kern base build completed without publishing the exact locked rootfs")
    _publish_staged_kern_image(
        kern,
        staging_image,
        image,
        lambda candidate: _base_image_ready(kern, lock, candidate),
    )


def _kern_build_user_ready(kern: str, image: str) -> bool:
    """Return whether the host can map the image's unprivileged package builder."""
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("doctor-build-user"),
                "--image",
                image,
                "--pull",
                "never",
                "--read-only",
                "--network",
                "none",
                "--tmpfs",
                "/tmp:16m",  # noqa: S108 -- disposable runtime probe.
                "--quiet",
                "--",
                "sh",
                "-ceu",
                (
                    "install -d -o builder -g builder /tmp/fplinux-builder; "
                    "su builder -s /bin/sh -c 'test -w /tmp/fplinux-builder; "
                    ": > /tmp/fplinux-builder/probe'"
                ),
            ],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=_KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return False
    return result.returncode == 0


def setup(
    *,
    force: bool = False,
    reporter: RunReporter | None = None,
    lock: dict[str, Any] | None = None,
    image_recipe: str | None = None,
) -> ImageState:
    own_reporter = reporter is None
    if reporter is None:
        reporter = RunReporter.create("setup", target=None, verbose=False)
    if lock is None:
        lock = load_container_lock()
    kern = _install_kern(lock)
    _prune_kern_build_history(kern)
    current_recipe = container_image_recipe_digest(lock)
    if image_recipe is not None and image_recipe != current_recipe:
        fail("container image inputs changed before setup")
    image_recipe = current_recipe
    image = container_image_reference(lock, image_recipe)
    _discard_transient_kern_images(kern, lock)
    current_state = current_image_state(kern, image, image_recipe)
    if current_state is not None and not force:
        state = publish_current_image_state(
            kern,
            image,
            image_recipe,
            state=current_state,
        )
        _discard_obsolete_kern_images(kern, lock, image_recipe)
        install_git_hooks()
        print(f"Build image is ready: {image}")
        if own_reporter:
            reporter.finish()
        return state

    if not _base_image_ready(kern, lock):
        _build_base_image(kern, reporter, lock)

    generation = secrets.token_hex(32)
    staging_image = _temporary_image_reference(image, "staging")
    for relative in (".kernignore", "Containerfile", "package.json", "package-lock.json"):
        source = ROOT / relative
        if source.is_symlink() or not source.is_file():
            fail(f"container image input is missing or invalid: {source}")
    command = [
        kern,
        "build",
        "-t",
        staging_image,
        *container_image_build_arguments(lock),
        "--build-arg",
        f"FPLINUX_IMAGE_RECIPE={image_recipe}",
        "--build-arg",
        f"FPLINUX_IMAGE_GENERATION={generation}",
        ".",
    ]
    with reporter.stage("container-setup") as stage:
        stage.run(
            command,
            cwd=ROOT,
            env=kern_environment(),
            timeout=_CONTAINER_SETUP_TIMEOUT,
        )
    if container_image_recipe_digest(lock) != image_recipe:
        fail("container image inputs changed while setup was running")
    if _image_metadata(kern, staging_image) != (image_recipe, generation):
        fail("container setup completed without publishing the exact requested image")
    _publish_staged_kern_image(
        kern,
        staging_image,
        image,
        lambda candidate: _image_metadata(kern, candidate) == (image_recipe, generation),
    )
    state = publish_current_image_state(kern, image, image_recipe)
    _discard_obsolete_kern_images(kern, lock, image_recipe)
    _prune_kern_build_history(kern)
    install_git_hooks()
    if own_reporter:
        reporter.finish()
    return state


def doctor() -> None:
    problems: list[str] = []
    print(f"host:     {platform.system()} {platform.machine()}")
    if platform.system() != "Linux":
        problems.append("the build interface currently supports Linux hosts only")
    if platform.machine() not in {"x86_64", "amd64"}:
        problems.append("the pinned build image currently targets linux/amd64")
    lock = load_container_lock()
    if not kern_available(lock):
        problems.append("Kern is not ready for this checkout; run ./fplinux setup")
    else:
        kern = require_kern(lock)
        try:
            version = subprocess.run(
                [kern, "--version"],
                cwd=ROOT,
                env=kern_environment(),
                capture_output=True,
                text=True,
                check=False,
                timeout=_KERN_PROBE_TIMEOUT,
            )
        except subprocess.TimeoutExpired:
            problems.append(f"Kern version timed out after {_KERN_PROBE_TIMEOUT}s")
        else:
            expected_version = f"kern {lock['kern']['version']}"
            if version.returncode or version.stdout.strip() != expected_version:
                problems.append(version.stderr.strip() or "unexpected Kern version")
            else:
                print(f"kern:      {lock['kern']['version']} (project-local)")
        try:
            runtime = subprocess.run(
                [kern, "doctor"],
                cwd=ROOT,
                env=kern_environment(),
                capture_output=True,
                text=True,
                check=False,
                timeout=_KERN_PROBE_TIMEOUT,
            )
        except subprocess.TimeoutExpired:
            problems.append(f"Kern doctor timed out after {_KERN_PROBE_TIMEOUT}s")
        else:
            if runtime.returncode:
                problems.append(
                    runtime.stderr.strip() or runtime.stdout.strip() or "Kern doctor failed"
                )
            else:
                print("runtime:   ready")
        image = container_image_reference(lock)
        ready = current_image_state(kern, image) is not None
        state = "ready" if ready else "not built or stale"
        print(f"image:     {state} ({image})")
        if not ready:
            problems.append("the pinned build image is not ready; run ./fplinux setup")
        elif not _kern_build_user_ready(kern, image):
            problems.append(
                "Kern cannot map the package builder; configure newuidmap/newgidmap "
                "and subordinate UID/GID ranges"
            )
    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        raise SystemExit(1)
    print("doctor: OK")


def check_git_diff(reporter: RunReporter) -> None:
    try:
        head = subprocess.run(
            ["git", "rev-parse", "--verify", "HEAD"],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=_CHECK_GIT_TIMEOUT,
        )
    except subprocess.TimeoutExpired as error:
        raise SystemExit(
            f"check failed: Git HEAD lookup timed out after {_CHECK_GIT_TIMEOUT}s"
        ) from error
    if head.returncode == 0:
        with reporter.stage("git-diff") as stage:
            stage.run(
                ["git", "diff", "--check", "HEAD", "--"],
                cwd=ROOT,
                timeout=_CHECK_GIT_TIMEOUT,
            )


def check_commit_message(message_file: str) -> None:
    """Validate one Git commit message in the pinned environment."""
    message = Path(message_file)
    if message.is_symlink() or not message.is_file():
        fail(f"commit message file is missing or invalid: {message}")
    config = ROOT / "commitlint.config.mjs"
    if config.is_symlink() or not config.is_file():
        fail("commitlint configuration is missing or invalid")
    container_lock = load_container_lock()
    kern = require_kern(container_lock)
    image = container_image_reference(container_lock)
    image_state = current_image_state(kern, image)
    if image_state is None:
        fail("commit hook requires the current build image; run ./fplinux setup")
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("commitlint"),
                "--image",
                image,
                "--pull",
                "never",
                "--read-only",
                "--network",
                "none",
                "--tmpfs",
                "/tmp:64m",  # noqa: S108 -- container tmpfs.
                "--no-uid-range",
                "--volume",
                f"{config}:/workspace/commitlint.config.mjs:ro",
                "--volume",
                f"{message.resolve()}:/message:ro",
                "--env",
                "HOME=/tmp",
                "--workdir",
                "/workspace",
                "--quiet",
                "--",
                "sh",
                "-c",
                "commitlint < /message",
            ],
            cwd=ROOT,
            env=kern_environment(),
            check=False,
            timeout=_COMMIT_MESSAGE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"commit message validation timed out after {_COMMIT_MESSAGE_TIMEOUT}s")
    if result.returncode:
        raise SystemExit(result.returncode)


def resolve_check_scopes(scopes: list[str]) -> tuple[str, ...]:
    """Validate, deduplicate and canonicalize a check selection."""
    requested = set(scopes)
    unknown = requested.difference(CHECK_SCOPES)
    if unknown:
        fail(f"unknown check scope: {', '.join(sorted(unknown))}")
    return tuple(scope for scope in CHECK_SCOPES if not scopes or scope in requested)


def analyzer_cache_names(scopes: tuple[str, ...]) -> tuple[str, ...]:
    """Return analyzer caches required by the selected scopes."""
    required: set[str] = set()
    if "kernel" in scopes:
        required.update(("analysis", "downloads", "linux"))
    return tuple(name for name in ("analysis", "downloads", "linux") if name in required)


def _is_shell_source(file: WorkspaceFile) -> bool:
    if Path(file.path).suffix not in {"", ".initd", ".sh"}:
        return False
    first_line = file.contents.splitlines()[:1]
    if not first_line:
        return False
    return shell_dialect(first_line[0]) is not None


def _is_prettier_configuration(path: str) -> bool:
    name = Path(path).name
    return (
        name in {".gitignore", ".prettierignore", "package.yaml"}
        or name in _PRETTIER_CONFIGURATION_NAMES
    )


def _source_scope_uses_file(  # noqa: PLR0911
    scope: str, file: WorkspaceFile
) -> bool:
    """Return whether one captured file can affect the selected source scope."""
    path = PurePath(file.path)
    name = path.name
    suffix = path.suffix.lower()
    parts = path.parts
    if file.path in _CHECK_IMPLEMENTATION:
        return True
    if scope in {
        "source",
        "docs",
        "spelling",
        "secrets",
        "licenses",
        "python",
    }:
        return True
    if scope == "container":
        return name in {".kernignore", "Containerfile"} or name.startswith(".hadolint")
    if scope == "metadata":
        return (
            suffix == ".toml"
            or (suffix in {".json", ".jsonc"} and name != "package-lock.json")
            or name == ".editorconfig"
            or _is_prettier_configuration(file.path)
        )
    if scope == "shell":
        return _is_shell_source(file) or name in {".editorconfig", ".shellcheckrc"}
    if scope == "alpine":
        return (
            file.path in {"alpine.lock.toml", "alpine/abuild.conf"}
            or (len(parts) == 3 and parts[0] == "targets" and name == "target.toml")
            or (len(parts) == 3 and parts[0] == "platforms" and name == "platform.toml")
            or (len(parts) == 4 and parts[:2] == ("alpine", "aports") and name == "APKBUILD")
        )
    if scope == "c":
        return (
            name in {".clang-format", ".clang-format-ignore", "_clang-format"}
            or (len(parts) == 3 and parts[0] == "targets" and name == "target.toml")
            or (len(parts) == 3 and parts[0] == "platforms" and name == "platform.toml")
        )
    fail(f"check scope does not support a source closure: {scope}")
    return False


def _c_scope_paths(snapshot: WorkspaceSnapshot) -> set[str]:
    """Resolve the same userspace/bootstrap C inputs and their quoted headers."""
    by_path = {file.path: file for file in snapshot.files}
    selected = {file.path for file in snapshot.files if _source_scope_uses_file("c", file)}
    for file in snapshot.files:
        path = PurePath(file.path)
        if path.suffix in {".c", ".h"} and (
            path.parts[:2] == ("alpine", "aports")
            or file.path in alpine_state.SHARED_APORT_SOURCE_PATHS
            or path.parts[0] == "tests"
        ):
            selected.add(file.path)
        if path.suffix in {".c", ".h"} and "bootstrap" in path.parts:
            selected.add(file.path)

    for file in snapshot.files:
        path = PurePath(file.path)
        if len(path.parts) != 3 or path.parts[0] != "platforms" or path.name != "platform.toml":
            continue
        try:
            manifest = tomllib.loads(file.contents.decode("utf-8"))
        except UnicodeDecodeError, tomllib.TOMLDecodeError:
            continue
        tools = manifest.get("host", {}).get("tools", [])
        if not isinstance(tools, list):
            continue
        for recipe in tools:
            if isinstance(recipe, dict) and recipe.get("type") == "cc-libusb":
                source = recipe.get("source")
                if isinstance(source, str) and source in by_path:
                    selected.add(source)

    pending = list(selected)
    while pending:
        relative = pending.pop()
        source = by_path.get(relative)
        if source is None or PurePath(relative).suffix not in {".c", ".h"}:
            continue
        for raw_include in _QUOTED_C_INCLUDE.findall(source.contents):
            try:
                include = raw_include.decode("utf-8")
            except UnicodeDecodeError:
                continue
            candidates = (
                (PurePath(relative).parent / include).as_posix(),
                PurePath(include).as_posix(),
            )
            for candidate in candidates:
                if candidate in by_path and candidate not in selected:
                    selected.add(candidate)
                    pending.append(candidate)
                    break
    return selected


def _linux_manifest_sources(linux: object, *, base: PurePath) -> set[str]:
    """Return the captured Linux inputs explicitly named by one manifest."""
    if not isinstance(linux, dict):
        return set()
    selected: set[str] = set()
    patches = linux.get("patches")
    if isinstance(patches, list):
        selected.update((base / patch).as_posix() for patch in patches if isinstance(patch, str))
    for key in ("copies", "appends"):
        steps = linux.get(key)
        if isinstance(steps, list):
            selected.update(
                (base / source).as_posix()
                for step in steps
                if isinstance(step, dict) and isinstance((source := step.get("source")), str)
            )
    return selected


def _profile_manifest_parts(path: PurePath) -> tuple[str, str] | None:
    """Return target/profile for one profile manifest path, if it has the fixed shape."""
    if (
        len(path.parts) == 5
        and path.parts[0] == "targets"
        and path.parts[2] == "profiles"
        and path.name == "profile.toml"
    ):
        return path.parts[1], path.parts[3]
    return None


def _kernel_scope_paths(snapshot: WorkspaceSnapshot, profile: str | None = None) -> set[str]:
    """Resolve default Linux inputs, or one explicitly named profile."""
    by_path = {file.path: file for file in snapshot.files}
    selected = {
        file.path
        for file in snapshot.files
        if file.path in _KERNEL_IMPLEMENTATION or file.path == "sources.lock.toml"
    }
    profiles_by_target: dict[str, list[WorkspaceFile]] = {}
    if profile is not None:
        for file in snapshot.files:
            parts = _profile_manifest_parts(PurePath(file.path))
            if parts is None:
                continue
            target_name, declared = parts
            if profile == declared:
                profiles_by_target.setdefault(target_name, []).append(file)

    target_manifests = [
        file
        for file in snapshot.files
        if (path := PurePath(file.path)).parts[:1] == ("targets",)
        and len(path.parts) == 3
        and path.name == "target.toml"
        and (profile is None or path.parts[1] in profiles_by_target)
    ]
    for target_manifest in target_manifests:
        target_path = PurePath(target_manifest.path)
        selected.add(target_manifest.path)
        try:
            target_data = tomllib.loads(target_manifest.contents.decode("utf-8"))
        except UnicodeDecodeError, tomllib.TOMLDecodeError:
            continue
        if not isinstance(target_data, dict):
            continue
        selected.add((target_path.parent / "kernel/defconfig").as_posix())
        selected.update(_linux_manifest_sources(target_data.get("linux"), base=target_path.parent))
        platform = target_data.get("platform")
        if not isinstance(platform, str):
            continue
        platform_path = PurePath("platforms") / platform / "platform.toml"
        platform_manifest = by_path.get(platform_path.as_posix())
        if platform_manifest is None:
            continue
        selected.add(platform_manifest.path)
        try:
            platform_data = tomllib.loads(platform_manifest.contents.decode("utf-8"))
        except UnicodeDecodeError, tomllib.TOMLDecodeError:
            continue
        if isinstance(platform_data, dict):
            selected.update(_linux_manifest_sources(platform_data.get("linux"), base=PurePath()))
        for profile_manifest in profiles_by_target.get(target_path.parent.name, []):
            profile_path = PurePath(profile_manifest.path)
            selected.add(profile_manifest.path)
            try:
                profile_data = tomllib.loads(profile_manifest.contents.decode("utf-8"))
            except UnicodeDecodeError, tomllib.TOMLDecodeError:
                continue
            if isinstance(profile_data, dict):
                selected.update(
                    _linux_manifest_sources(profile_data.get("linux"), base=profile_path.parent)
                )
    return selected


def check_scope_closure_digest(
    scope: str, snapshot: WorkspaceSnapshot, *, profile: str | None = None
) -> str:
    """Hash only captured files that can affect one exact check scope."""
    if scope == "kernel":
        paths = _kernel_scope_paths(snapshot, profile)
        selected = [file for file in snapshot.files if file.path in paths]
    elif scope == "c":
        paths = _c_scope_paths(snapshot)
        selected = [file for file in snapshot.files if file.path in paths]
    elif scope in SOURCE_CHECK_SCOPES:
        broaden = (
            scope == "metadata"
            and any(
                Path(file.path).name in _EXECUTABLE_PRETTIER_CONFIGURATION_NAMES
                for file in snapshot.files
            )
        ) or (
            scope == "shell"
            and any(
                file.path == ".shellcheckrc" and b"external-sources=true" in file.contents
                for file in snapshot.files
            )
        )
        selected = [
            file for file in snapshot.files if broaden or _source_scope_uses_file(scope, file)
        ]
    else:
        fail(f"check scope does not support receipts: {scope}")
    if not selected:
        fail(f"check scope has an empty causal closure: {scope}")
    return check_closure_entries_digest(
        [(file.path, file.contents, file.mode) for file in selected]
    )


def check_scope_commands(scope: str, *, profile: str | None = None) -> tuple[tuple[str, ...], ...]:
    """Return semantic checker argv without execution-only scheduling flags."""
    if scope in SOURCE_CHECK_SCOPES:
        return ((*_SOURCE_CHECK_COMMAND, scope),)
    if scope == "kernel":
        if profile is None:
            return _KERNEL_CHECK_COMMANDS
        return tuple((*command, "--profile", profile) for command in _KERNEL_CHECK_COMMANDS)
    fail(f"check scope does not support receipts: {scope}")
    return ()


def check_scope_receipt_recipe(  # noqa: PLR0913 -- receipt identities are distinct inputs.
    scope: str,
    closure_digest: str,
    *,
    image_generation: str,
    commands: tuple[tuple[str, ...], ...] | None = None,
    orchestration_recipe: str | None = None,
    profile: str | None = None,
) -> CheckReceiptRecipe:
    """Bind one cacheable source scope to its exact closure and OCI identities."""
    if scope not in (*SOURCE_CHECK_SCOPES, "kernel"):
        fail(f"check scope does not support receipts: {scope}")
    if commands is None:
        commands = check_scope_commands(scope, profile=profile)
    if orchestration_recipe is None:
        orchestration_recipe = check_orchestration_recipe_digest()
    return CheckReceiptRecipe(
        scope=scope,
        closure_digest=closure_digest,
        orchestration_recipe=orchestration_recipe,
        image_generation=image_generation,
        commands=commands,
        profile=profile,
    )


def _run_missing_checks(  # noqa: PLR0913 -- container boundaries are explicit.
    *,
    reporter: RunReporter,
    cache: Path,
    missing: tuple[str, ...],
    analyzer_cache: dict[str, Path],
    workspace: Path,
    kern: str,
    image: str,
    recipes: dict[str, CheckReceiptRecipe],
    profile: str | None,
    jobs: int,
) -> None:
    """Run cache-missing source and kernel checks against one disposable workspace."""
    container_logs = reporter.root / "containers"
    if container_logs.is_symlink() or (container_logs.exists() and not container_logs.is_dir()):
        fail(f"invalid checker container log directory: {container_logs}")
    container_logs.mkdir(parents=True, exist_ok=True)
    environment = kern_environment()
    log_mount = ["--volume", f"{container_logs}:/logs"]
    source_scopes = [scope for scope in missing if scope in SOURCE_CHECK_SCOPES]
    if source_scopes:
        log_environment = reporter.container_environment("/logs/source")
        log_environment["FPLINUX_LOG_DISPLAY_ROOT"] = (
            f"{log_environment['FPLINUX_LOG_DISPLAY_ROOT']}/containers/source"
        )
        log_arguments = [
            argument
            for key, value in log_environment.items()
            for argument in ("--env", f"{key}={value}")
        ]
        with reporter.stage("source", passthrough=True, show_tail=False) as stage:
            stage.run(
                [
                    kern,
                    "box",
                    kern_box_name("check-source"),
                    "--image",
                    image,
                    "--pull",
                    "never",
                    "--read-only",
                    "--network",
                    "none",
                    "--tmpfs",
                    "/tmp:1g",  # noqa: S108 -- container tmpfs.
                    "--no-uid-range",
                    "--volume",
                    f"{workspace}:/workspace:ro",
                    *log_mount,
                    *log_arguments,
                    "--env",
                    "HOME=/tmp",
                    "--env",
                    "PYTHONPATH=/workspace/scripts",
                    "--env",
                    "RUFF_CACHE_DIR=/tmp/ruff",
                    "--env",
                    "PYTHONDONTWRITEBYTECODE=1",
                    "--workdir",
                    "/workspace",
                    "--init",
                    "--quiet",
                    "--",
                    "python3",
                    "/workspace/scripts/check.py",
                    *source_scopes,
                ],
                env=environment,
                timeout=_SOURCE_CHECK_TIMEOUT,
            )
        for scope in source_scopes:
            publish_success_receipt(cache, recipes[scope])

    if "kernel" not in missing:
        return
    log_environment = reporter.container_environment("/logs/kernel")
    log_environment["FPLINUX_LOG_DISPLAY_ROOT"] = (
        f"{log_environment['FPLINUX_LOG_DISPLAY_ROOT']}/containers/kernel"
    )
    log_arguments = [
        argument
        for key, value in log_environment.items()
        for argument in ("--env", f"{key}={value}")
    ]
    common = [
        "--image",
        image,
        "--pull",
        "never",
        "--read-only",
        "--tmpfs",
        "/tmp:8g",  # noqa: S108 -- container tmpfs.
        "--no-uid-range",
        "--volume",
        f"{workspace}:/workspace:ro",
        *log_mount,
        *log_arguments,
        "--env",
        "HOME=/tmp",
        "--env",
        "PYTHONPATH=/workspace/scripts",
        "--env",
        "PYTHONDONTWRITEBYTECODE=1",
        "--workdir",
        "/workspace",
        "--init",
        "--quiet",
    ]
    with reporter.stage("kernel-prepare", passthrough=True, show_tail=False) as stage:
        stage.run(
            [
                kern,
                "box",
                kern_box_name("kernel-prepare"),
                *common,
                "--network",
                "host",
                "--volume",
                f"{analyzer_cache['downloads']}:/cache/downloads",
                "--volume",
                f"{analyzer_cache['linux']}:/cache/linux",
                "--",
                "python3",
                "-m",
                "fplinux_cli.kernelcheck",
                "prepare",
                *([] if profile is None else ["--profile", profile]),
            ],
            env=environment,
            timeout=_KERNEL_PREPARE_TIMEOUT,
        )
    with reporter.stage("kernel-analysis", passthrough=True, show_tail=False) as stage:
        stage.run(
            [
                kern,
                "box",
                kern_box_name("kernel-analysis"),
                *common,
                "--network",
                "none",
                "--volume",
                f"{analyzer_cache['analysis']}:/cache/analysis",
                "--volume",
                f"{analyzer_cache['linux']}:/cache/linux:ro",
                "--",
                "python3",
                "-m",
                "fplinux_cli.kernelcheck",
                "check",
                "--jobs",
                str(jobs),
                *([] if profile is None else ["--profile", profile]),
            ],
            env=environment,
            timeout=_KERNEL_ANALYSIS_TIMEOUT,
        )
    publish_success_receipt(cache, recipes["kernel"])


def check(
    scopes: list[str],
    *,
    verbose: bool = False,
    no_cache: bool = False,
    profile: str | None = None,
    jobs: int = 1,
) -> None:
    if not isinstance(jobs, int) or isinstance(jobs, bool) or jobs < 1:
        fail("--jobs must be a positive integer")
    selected = resolve_check_scopes(scopes)
    if profile is not None:
        declared_targets = tuple(
            target for target in discover_targets() if profile in discover_profiles(target)
        )
        if not declared_targets:
            fail(f"check profile is not declared by any target: {profile}")
        if not scopes:
            selected = ("kernel",)
        elif selected != ("kernel",):
            fail("--profile is supported only with the kernel check scope")
    if jobs > 1 and verbose:
        fail("--verbose cannot be combined with --jobs greater than 1")

    reporter = RunReporter.create(
        "check",
        target=None if profile is None else f"profiles/{profile}",
        verbose=verbose,
    )
    if "repository" in selected:
        check_git_diff(reporter)
    if selected == ("repository",):
        print("check: OK")
        reporter.finish()
        return

    with reporter.stage("workspace-snapshot"):
        snapshot = quality_workspace_snapshot(enforce_source_policy="source" in selected)

    cache = ROOT / ".cache"
    cacheable_scopes = tuple(scope for scope in selected if scope != "repository")
    container_lock = load_container_lock()
    image_recipe = container_image_recipe_digest(container_lock)
    orchestration_recipe = check_orchestration_recipe_digest(image_recipe)

    def receipt_recipes(image_generation: str) -> dict[str, CheckReceiptRecipe]:
        return {
            scope: check_scope_receipt_recipe(
                scope,
                check_scope_closure_digest(scope, snapshot, profile=profile),
                image_generation=image_generation,
                orchestration_recipe=orchestration_recipe,
                profile=profile,
            )
            for scope in cacheable_scopes
        }

    cached_image = load_image_state(cache, image_recipe)
    if cached_image is not None:
        recipes = receipt_recipes(cached_image.image_generation)
        missing = tuple(
            scope
            for scope in cacheable_scopes
            if no_cache or not receipt_matches(cache, recipes[scope])
        )
        if not missing:
            for scope in cacheable_scopes:
                print(f"check cache: hit ({scope})")
            print("check: OK")
            reporter.finish()
            return

    image = container_image_reference(container_lock, image_recipe)
    if kern_available(container_lock):
        kern = require_kern(container_lock)
        inspected_image = current_image_state(kern, image, image_recipe)
        if inspected_image is not None:
            current_image = publish_current_image_state(
                kern,
                image,
                image_recipe,
                state=inspected_image,
            )
        else:
            current_image = setup(
                reporter=reporter,
                lock=container_lock,
                image_recipe=image_recipe,
            )
    else:
        current_image = setup(
            reporter=reporter,
            lock=container_lock,
            image_recipe=image_recipe,
        )
        kern = require_kern(container_lock)

    recipes = receipt_recipes(current_image.image_generation)
    missing = tuple(
        scope
        for scope in cacheable_scopes
        if no_cache or not receipt_matches(cache, recipes[scope])
    )
    for scope in cacheable_scopes:
        if scope not in missing:
            print(f"check cache: hit ({scope})")
    if not missing:
        print("check: OK")
        reporter.finish()
        return

    analyzer_cache: dict[str, Path] = {}
    for name in analyzer_cache_names(missing):
        source = cache / name
        if source.is_symlink() or (source.exists() and not source.is_dir()):
            fail(f"invalid analyzer cache path: {source}")
        source.mkdir(parents=True, exist_ok=True)
        analyzer_cache[name] = source

    with reporter.stage("workspace"):
        workspace = stage_quality_workspace_snapshot(snapshot)

    try:
        _run_missing_checks(
            reporter=reporter,
            cache=cache,
            missing=missing,
            analyzer_cache=analyzer_cache,
            workspace=workspace,
            kern=kern,
            image=image,
            recipes=recipes,
            profile=profile,
            jobs=jobs,
        )
    finally:
        discard_staged_quality_workspace_snapshot(snapshot, workspace)
    print("check: OK")
    reporter.finish()
    if profile is not None:
        discard_superseded_profile_logs(cache, "check", profile=profile)
