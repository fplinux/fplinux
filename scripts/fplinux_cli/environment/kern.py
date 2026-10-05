# SPDX-License-Identifier: GPL-2.0-only
"""Manage the project-local Kern runtime and build image."""

from __future__ import annotations

import hashlib
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
import urllib.request
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import ROOT, alpine_tar_filter, error_message, fail, sha256_file
from fplinux_cli.dependency_inputs import environment_inputs
from fplinux_cli.environment.images import (
    container_base_image_reference,
    container_image_build_arguments,
    container_image_recipe_digest,
    container_image_reference,
    load_container_lock,
)
from fplinux_cli.image_state import ImageState, ImageStateError, publish_image_state
from fplinux_cli.output import RunReporter

from .git_hooks import install_git_hooks

if TYPE_CHECKING:
    from collections.abc import Callable

_KERN_PROBE_TIMEOUT = 60


_CONTAINER_SETUP_TIMEOUT = 2 * 60 * 60


_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


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


def _download_locked_file(  # noqa: PLR0913 -- checksum, size and offline policy are distinct inputs.
    url: str,
    digest: str,
    destination: Path,
    *,
    offline: bool = False,
    algorithm: str = "sha256",
    size: int | None = None,
) -> Path:
    """Fetch one declared checksum match atomically, or require saved bytes offline."""
    if not url.startswith("https://"):
        fail("locked download URL must use HTTPS")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() or (destination.exists() and not destination.is_file()):
        fail(f"invalid locked download path: {destination}")
    if (
        destination.is_file()
        and (size is None or destination.stat().st_size == size)
        and _locked_file_digest(destination, algorithm=algorithm) == digest
    ):
        return destination
    if offline:
        fail(f"offline locked input is missing or mismatched: {url}: {destination}")
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
        actual = _locked_file_digest(temporary, algorithm=algorithm)
        if actual != digest:
            fail(
                f"locked download {algorithm.upper()} mismatch: {url}: "
                f"expected {digest}, received {actual}"
            )
        if size is not None and temporary.stat().st_size != size:
            fail(f"locked download size mismatch: {url}: expected {size}")
        temporary.replace(destination)
        temporary = None
    except OSError as error:
        fail(f"locked download failed: {url}: {error}")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return destination


def _locked_file_digest(path: Path, *, algorithm: str) -> str:
    """Use the checksum algorithm declared for the original downloaded artifact."""
    if algorithm == "sha256":
        return sha256_file(path)
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def _install_kern(lock: dict[str, Any], *, offline: bool = False) -> str:
    """Install the pinned static Kern binary under the project cache."""
    if kern_available(lock):
        return str(_kern_path())
    kern_lock = lock["kern"]
    archive = _download_locked_file(
        kern_lock["archive_url"],
        kern_lock["archive_sha256"],
        ROOT / ".cache/downloads/kern/kern.tar.gz",
        offline=offline,
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
            # A flat image copy must retain ownership and set-id permissions.
            ["unshare", "--map-auto", "--map-root-user", "--", kern, "tag", source, destination],
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


def _image_metadata(kern: str, image: str) -> tuple[str, str, str] | None:
    """Require the declared recipe, generation and freshly checked installed content."""
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
                "--quiet",
                "--",
                "sh",
                "-ceu",
                (
                    "cat /etc/fplinux-image-state; "
                    "python3 -B /usr/local/libexec/fplinux-image-content.py"
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
        fail(f"Kern image lookup timed out after {_KERN_PROBE_TIMEOUT}s")
    lines = result.stdout.splitlines()
    if result.returncode != 0 or len(lines) != 4:
        return None
    recipe, generation, content, observed_content = lines
    if _SHA256.fullmatch(recipe) is None:
        return None
    if _SHA256.fullmatch(generation) is None:
        return None
    if _SHA256.fullmatch(content) is None or observed_content != content:
        return None
    return recipe, generation, content


def image_generation(kern: str, image: str) -> str | None:
    """Return the exact build generation embedded in one project-built Kern image."""
    metadata = _image_metadata(kern, image)
    return None if metadata is None else metadata[1]


def current_image_state(
    kern: str,
    image: str,
    image_recipe: str | None = None,
) -> ImageState | None:
    """Read one valid recipe, generation and content identity with a single Kern probe."""
    metadata = _image_metadata(kern, image)
    if metadata is None:
        return None
    if image_recipe is None:
        image_recipe = container_image_recipe_digest()
    recipe, generation, content = metadata
    if recipe != image_recipe:
        return None
    try:
        return ImageState(recipe, generation, content)
    except ImageStateError:
        return None


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


_alpine_tar_filter = partial(alpine_tar_filter, on_error=fail)


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


def _build_base_image(
    kern: str, reporter: RunReporter, lock: dict[str, Any], *, offline: bool = False
) -> None:
    """Build one local Kern base from the exact official Alpine minirootfs archive."""
    oci = lock["oci"]
    image = container_base_image_reference(lock)
    staging_image = _temporary_image_reference(image, "staging")
    archive = _download_locked_file(
        oci["base_rootfs_url"],
        oci["base_rootfs_sha256"],
        ROOT / ".cache/downloads/kern/alpine-minirootfs.tar.gz",
        offline=offline,
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


def _stage_container_context(context: Path, *, offline: bool) -> None:
    """Stage the recipe and exact saved inputs without copying unrelated cache files."""
    for relative in (
        ".kernignore",
        "Containerfile",
        "scripts/fplinux_cli/image_content.py",
        "package.json",
        "package-lock.json",
        "alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch",
    ):
        source = ROOT / relative
        if source.is_symlink() or not source.is_file():
            fail(f"container image input is missing or invalid: {source}")
        destination = context / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    input_directory = context / "inputs"
    input_directory.mkdir()
    for item in environment_inputs(ROOT):
        if item.purpose in {"container-base", "kern-runtime"}:
            continue
        expected = item.checksum or item.sha256
        if expected is None:
            fail(f"environment input has no declared checksum: {item.key}")
        source = _download_locked_file(
            item.url,
            expected,
            ROOT / ".cache" / item.destination,
            offline=offline,
            algorithm=item.algorithm,
            size=item.size,
        )
        if item.purpose == "npm-package":
            relative_input = Path("npm") / source.name
        elif item.purpose == "container-source":
            relative_input = Path("sources") / Path(item.destination).relative_to(
                "downloads/environment"
            )
        else:
            relative_input = Path(item.destination).relative_to("downloads/environment")
        destination = input_directory / relative_input
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def setup(
    *,
    force: bool = False,
    offline: bool = False,
    reporter: RunReporter | None = None,
    lock: dict[str, Any] | None = None,
    image_recipe: str | None = None,
) -> ImageState:
    own_reporter = reporter is None
    if reporter is None:
        reporter = RunReporter.create("setup", target=None, verbose=False)
    if lock is None:
        lock = load_container_lock()
    kern = _install_kern(lock, offline=offline)
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
        _build_base_image(kern, reporter, lock, offline=offline)

    generation = secrets.token_hex(32)
    staging_image = _temporary_image_reference(image, "staging")
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
    ]
    command.append(".")
    temporary_parent = _ensure_project_directory(ROOT / ".cache/kern")
    with tempfile.TemporaryDirectory(dir=temporary_parent, prefix="image-build-") as temporary:
        context = Path(temporary)
        with reporter.stage("container-inputs"):
            _stage_container_context(context, offline=offline)
        with reporter.stage("container-setup") as stage:
            stage.run(
                command,
                cwd=context,
                env=kern_environment(),
                timeout=_CONTAINER_SETUP_TIMEOUT,
            )
    if container_image_recipe_digest(lock) != image_recipe:
        fail("container image inputs changed while setup was running")
    metadata = _image_metadata(kern, staging_image)
    if metadata is None or metadata[:2] != (image_recipe, generation):
        fail("container setup completed without publishing the exact requested image")
    _publish_staged_kern_image(
        kern,
        staging_image,
        image,
        lambda candidate: _image_metadata(kern, candidate) == metadata,
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
            print(error_message(problem), file=sys.stderr)
        raise SystemExit(1)
    print("doctor: OK")
